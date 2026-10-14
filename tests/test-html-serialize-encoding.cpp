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
 * The HTML serializer in the encodings of the Encoding Standard (phase 3j): a character that the encoding cannot write is a numeric
 * character reference where a reference is read as one (text, attribute values) and GTEXT_HTML_E_UNREPRESENTABLE where it is not (a
 * comment, a name, a document type, a processing instruction, raw text), with the sink having been given the output up to it; UTF-16
 * means UTF-8; an encoding with a state is back in ASCII before a reference and at the end; `inject_meta_charset` declares the name of
 * the encoding. And the round trip: bytes in an encoding, parsed, written in the same encoding, parsed again, give one tree.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/encoding.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/xml.h>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "html_api_test_support.h"
#include "markup_test_support.h"

using namespace html_api_test;

namespace {

struct Collect {
  std::string bytes;
  size_t calls = 0;
  size_t refuse_after = SIZE_MAX;
  GTEXT_Markup_Sink sink() {
    return GTEXT_Markup_Sink{[](void * user, const char * b, size_t n) -> GTEXT_Markup_Status {
                               auto * c = static_cast<Collect *>(user);
                               if (c->calls >= c->refuse_after) {
                                 return GTEXT_MARKUP_E_WRITE;
                               }
                               c->calls++;
                               c->bytes.append(b, n);
                               return GTEXT_MARKUP_OK;
                             },
        this};
  }
};

struct Written {
  GTEXT_HTML_Status status = GTEXT_HTML_OK;
  std::string bytes;
  GTEXT_HTML_Serialize_Result result;
};

/// Serialize the children of a document or fragment parsed from UTF-8 `html` as a fragment in a `body`.
struct Tree {
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  explicit Tree(const std::string & html, const char * context = "body") {
    GTEXT_HTML_Fragment_Context c;
    std::memset(&c, 0, sizeof(c));
    c.local_name = context;
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    EXPECT_EQ(gtext_html_parse_fragment(&c, html.data(), html.size(), &o, &doc, &frag, nullptr), GTEXT_HTML_OK);
  }
  ~Tree() {
    gtext_markup_document_free(doc);
  }
};

Written write(const GTEXT_Markup_Node * node, const char * encoding, bool scripting = false, size_t limit = 0) {
  Written w;
  GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
  o.encoding = encoding;
  o.scripting = scripting;
  o.max_output_bytes = limit;
  Collect c;
  GTEXT_Markup_Sink sink = c.sink();
  w.status = gtext_html_serialize(node, GTEXT_HTML_SERIALIZE_CHILDREN, &o, &sink, &w.result);
  w.bytes = c.bytes;
  return w;
}

std::string hex(const std::string & s) {
  std::string out;
  char b[4];
  for (unsigned char c : s) {
    std::snprintf(b, sizeof(b), "%02x", c);
    out += b;
  }
  return out;
}

} // namespace

TEST(HtmlSerializeEncoding, AReferenceInTextAndAnErrorInRawText) {
  // U+20AC is byte 0x80 in windows-1252 and has no place in ISO-8859-2.
  {
    Tree t("<p>a\xE2\x82\xAC" "b</p>");
    Written w = write(t.frag, "iso-8859-2");
    ASSERT_EQ(w.status, GTEXT_HTML_OK);
    EXPECT_EQ(w.bytes, "<p>a&#8364;b</p>");
    EXPECT_EQ(w.result.references_written, 1u);
    EXPECT_STREQ(w.result.encoding, "ISO-8859-2");
    EXPECT_FALSE(w.result.encoding_substituted);
    w = write(t.frag, "windows-1252");
    ASSERT_EQ(w.status, GTEXT_HTML_OK);
    EXPECT_EQ(w.bytes, "<p>a\x80" "b</p>");
    EXPECT_EQ(w.result.references_written, 0u);
  }
  // ... in a script, the same character has no spelling, and the sink has the output up to it
  {
    Tree t("<script>var a = 1; \xE2\x82\xAC; var b = 2;</script>", "div");
    Written w = write(t.frag, "iso-8859-2");
    EXPECT_EQ(w.status, GTEXT_HTML_E_UNREPRESENTABLE);
    EXPECT_EQ(w.result.status, GTEXT_HTML_E_UNREPRESENTABLE);
    EXPECT_EQ(w.result.unrepresentable_code_point, 0x20ACu);
    EXPECT_EQ(w.bytes, "<script>var a = 1; ");
    EXPECT_STREQ(gtext_html_status_string(GTEXT_HTML_E_UNREPRESENTABLE), "a character the output encoding cannot write here");
    // ... and a character it can write is not a problem there
    Tree u("<script>var a = '\xC4\x9B';</script>", "div");
    w = write(u.frag, "iso-8859-2");
    ASSERT_EQ(w.status, GTEXT_HTML_OK);
    EXPECT_EQ(w.bytes, "<script>var a = '\xEC';</script>");
  }
}

TEST(HtmlSerializeEncoding, EveryPlaceAReferenceIsReadAsAReferenceOrNot) {
  struct Row {
    const char * what;
    const char * html;
    const char * context;
    bool scripting;
    bool reference; // true: a reference is written; false: an error
  };
  const std::string euro = "\xE2\x82\xAC";
  const Row rows[] = {
      {"text", "<p>@</p>", "div", false, true},
      {"an attribute value, quoted", "<p title=\"@\">x</p>", "div", false, true},
      {"an attribute value, unquoted", "<p title=@>x</p>", "div", false, true},
      {"textarea (RCDATA: a reference is read in it)", "<textarea>@</textarea>", "div", false, true},
      {"title (RCDATA)", "<title>@</title>", "div", false, true},
      {"a comment", "<!--@-->", "div", false, false},
      {"a script", "<script>@</script>", "div", false, false},
      {"a style", "<style>@</style>", "div", false, false},
      {"an xmp", "<xmp>@</xmp>", "div", false, false},
      {"an iframe", "<iframe>@</iframe>", "div", false, false},
      {"a noembed", "<noembed>@</noembed>", "div", false, false},
      {"a noframes", "<noframes>@</noframes>", "div", false, false},
      {"a plaintext", "<plaintext>@", "div", false, false},
      {"a noscript with scripting", "<noscript>@</noscript>", "div", true, false},
      {"a noscript without it is text", "<noscript><b>@</b></noscript>", "div", false, true},
      {"a tag name", "<@-tag>x</@-tag>", "div", false, false},
      {"an attribute name", "<p @-attr=1>x</p>", "div", false, false},
      {"a processing instruction's data", "<?pi @?>", "div", false, false},
      {"a processing instruction's target", "<?@ data?>", "div", false, false},
  };
  for (const Row & row : rows) {
    SCOPED_TRACE(row.what);
    std::string html;
    for (const char * p = row.html; *p; p++) {
      if (*p == '@') {
        html += euro;
      } else {
        html += *p;
      }
    }
    Tree t(html, row.context);
    // The Euro sign is not in ISO-8859-2.
    const Written w = write(t.frag, "iso-8859-2", row.scripting);
    if (row.reference) {
      EXPECT_EQ(w.status, GTEXT_HTML_OK);
      EXPECT_NE(w.bytes.find("&#8364;"), std::string::npos) << w.bytes;
      EXPECT_EQ(w.result.references_written, 1u);
    } else {
      EXPECT_EQ(w.status, GTEXT_HTML_E_UNREPRESENTABLE) << w.bytes;
      EXPECT_EQ(w.result.unrepresentable_code_point, 0x20ACu);
      EXPECT_EQ(w.bytes.find('\xE2'), std::string::npos) << "the sink never sees the character in another encoding";
    }
  }
  // a document type: a name that has no place in the encoding
  {
    GTEXT_Markup_Document * doc = nullptr;
    ASSERT_EQ(gtext_markup_document_new(nullptr, &doc), GTEXT_MARKUP_OK);
    GTEXT_Markup_Node * dt = nullptr;
    ASSERT_EQ(gtext_markup_create_doctype(doc, "h\xE2\x82\xAC", nullptr, nullptr, &dt), GTEXT_MARKUP_OK);
    ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(doc), dt), GTEXT_MARKUP_OK);
    GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
    o.encoding = "iso-8859-2";
    Collect c;
    GTEXT_Markup_Sink sink = c.sink();
    GTEXT_HTML_Serialize_Result r;
    EXPECT_EQ(gtext_html_serialize(gtext_markup_document_root(doc), GTEXT_HTML_SERIALIZE_CHILDREN, &o, &sink, &r),
        GTEXT_HTML_E_UNREPRESENTABLE);
    EXPECT_EQ(c.bytes, "<!DOCTYPE h");
    gtext_markup_document_free(doc);
  }
}

TEST(HtmlSerializeEncoding, Utf16MeansUtf8AndReportsIt) {
  Tree t("<p title=\"\xE2\x82\xAC\">\xE2\x82\xAC</p>");
  for (const char * label : {"utf-16", "utf-16le", "utf-16be", "unicodefffe", "hz-gb-2312", "replacement"}) {
    SCOPED_TRACE(label);
    const Written w = write(t.frag, label);
    ASSERT_EQ(w.status, GTEXT_HTML_OK);
    EXPECT_EQ(w.bytes, "<p title=\"\xE2\x82\xAC\">\xE2\x82\xAC</p>");
    EXPECT_STREQ(w.result.encoding, "UTF-8");
    EXPECT_TRUE(w.result.encoding_substituted);
  }
  const Written plain = write(t.frag, nullptr);
  EXPECT_STREQ(plain.result.encoding, "UTF-8");
  EXPECT_FALSE(plain.result.encoding_substituted);
  // no encoder, no BOM either
  EXPECT_NE(write(t.frag, "utf-16le").bytes.substr(0, 2), std::string("\xFF\xFE", 2));
}

TEST(HtmlSerializeEncoding, TheEncodingsOfTheStandard) {
  const std::string text = "\xE6\x97\xA5\xE6\x9C\xAC"; // the Japanese word for Japan
  struct Row {
    const char * label;
    const char * name;
    std::string bytes; // the text in the encoding
  };
  const Row rows[] = {
      {"shift_jis", "Shift_JIS", "\x93\xFA\x96\x7B"},
      {"euc-jp", "EUC-JP", "\xC6\xFC\xCB\xDC"},
      {"gbk", "GBK", "\xC8\xD5\xB1\xBE"},
      {"gb18030", "gb18030", "\xC8\xD5\xB1\xBE"},
      {"big5", "Big5", "\xA4\xE9\xA5\xBB"},
      {"iso-2022-jp", "ISO-2022-JP", "\x1B$B" "F|K\\" "\x1B(B"},
  };
  for (const Row & row : rows) {
    SCOPED_TRACE(row.label);
    Tree t("<p>" + text + "</p>");
    const Written w = write(t.frag, row.label);
    ASSERT_EQ(w.status, GTEXT_HTML_OK) << w.result.message;
    EXPECT_STREQ(w.result.encoding, row.name);
    const std::string want = "<p>" + (row.bytes.empty() ? text : row.bytes) + "</p>";
    EXPECT_EQ(hex(w.bytes), hex(want));
  }
  // EUC-KR cannot write these characters; in text they are references
  Tree k("<p>" + text + "</p>");
  const Written kr = write(k.frag, "euc-kr");
  ASSERT_EQ(kr.status, GTEXT_HTML_OK);
  EXPECT_NE(kr.bytes.find('\xEC'), std::string::npos) << hex(kr.bytes); // hanja, as the encoding has them
}

TEST(HtmlSerializeEncoding, Iso2022JpGoesBackToAsciiBeforeAReferenceAndAtTheEnd) {
  // text: Japanese, a Euro sign (not in JIS X 0208), more Japanese, then an ASCII tag
  Tree t("<p>\xE6\x97\xA5\xE2\x82\xAC\xE6\x97\xA5</p><b>x</b>");
  const Written w = write(t.frag, "iso-2022-jp");
  ASSERT_EQ(w.status, GTEXT_HTML_OK);
  // <p> then ESC $ B (the character) ESC ( B (back to ASCII before the reference) &#8364; ESC $ B (the character) ESC ( B </p>...
  EXPECT_EQ(w.bytes, "<p>\x1B$BF|\x1B(B&#8364;\x1B$BF|\x1B(B</p><b>x</b>");
  // the text ends in a Japanese character, and nothing follows it: the output still ends in ASCII mode (the end of the stream)
  Tree u("\xE6\x97\xA5");
  const Written e = write(u.frag, "iso-2022-jp");
  ASSERT_EQ(e.status, GTEXT_HTML_OK);
  EXPECT_EQ(e.bytes, "\x1B$BF|\x1B(B");
  EXPECT_EQ(e.bytes.substr(e.bytes.size() - 3), "\x1B(B");
  // a comment (no references) after Japanese text: back to ASCII, and the character that cannot be written is an error
  Tree c("<p>\xE6\x97\xA5</p><!--\xE2\x82\xAC-->");
  const Written bad = write(c.frag, "iso-2022-jp");
  EXPECT_EQ(bad.status, GTEXT_HTML_E_UNREPRESENTABLE);
  EXPECT_EQ(bad.bytes, "<p>\x1B$BF|\x1B(B</p><!--") << "the escape back is written before the ASCII that follows, as the encoder does";
}

TEST(HtmlSerializeEncoding, TheOutputLimitCountsTheEncodedBytes) {
  Tree t("<p>\xE6\x97\xA5\xE6\x9C\xAC\xE6\x97\xA5\xE6\x9C\xAC</p>");
  const Written whole = write(t.frag, "shift_jis");
  ASSERT_EQ(whole.status, GTEXT_HTML_OK);
  ASSERT_EQ(whole.bytes.size(), 3 + 8 + 4u);
  EXPECT_EQ(write(t.frag, "shift_jis", false, whole.bytes.size()).status, GTEXT_HTML_OK);
  const Written under = write(t.frag, "shift_jis", false, whole.bytes.size() - 1);
  EXPECT_EQ(under.status, GTEXT_HTML_E_LIMIT_OUTPUT);
  EXPECT_LE(under.bytes.size(), whole.bytes.size() - 1);
  EXPECT_EQ(whole.bytes.compare(0, under.bytes.size(), under.bytes), 0) << "a prefix";
  EXPECT_EQ(under.result.bytes_written, under.bytes.size());
  // a sink that refuses
  GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
  o.encoding = "shift_jis";
  Collect c;
  c.refuse_after = 0;
  GTEXT_Markup_Sink sink = c.sink();
  EXPECT_EQ(gtext_html_serialize(t.frag, GTEXT_HTML_SERIALIZE_CHILDREN, &o, &sink, nullptr), GTEXT_HTML_E_WRITE);
}

TEST(HtmlSerializeEncoding, InjectMetaCharsetDeclaresTheName) {
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result pr;
  std::memset(&pr, 0, sizeof(pr));
  const std::string html = "<!DOCTYPE html><html><head><title>\xE6\x97\xA5</title></head><body>\xE6\x97\xA5</body></html>";
  GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
  ASSERT_EQ(gtext_html_parse(html.data(), html.size(), &po, &doc, &pr), GTEXT_HTML_OK);
  gtext_html_result_free(&pr);
  GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
  o.encoding = "Shift_JIS";
  o.inject_meta_charset = true;
  Collect c;
  GTEXT_Markup_Sink sink = c.sink();
  GTEXT_HTML_Serialize_Result r;
  ASSERT_EQ(gtext_html_serialize(gtext_markup_document_root(doc), GTEXT_HTML_SERIALIZE_CHILDREN, &o, &sink, &r), GTEXT_HTML_OK);
  EXPECT_EQ(r.meta_charset_injected, 1u);
  EXPECT_NE(c.bytes.find("<head><meta charset=\"shift_jis\"><title>\x93\xFA</title>"), std::string::npos) << hex(c.bytes);
  // what it wrote declares what it is in: parsed again, the document is in Shift_JIS without being told
  GTEXT_Markup_Document * again = nullptr;
  std::memset(&pr, 0, sizeof(pr));
  ASSERT_EQ(gtext_html_parse(c.bytes.data(), c.bytes.size(), &po, &again, &pr), GTEXT_HTML_OK);
  EXPECT_STREQ(pr.encoding, "Shift_JIS");
  EXPECT_EQ(pr.encoding_source, GTEXT_MARKUP_ENCODING_DECLARATION);
  gtext_html_result_free(&pr);
  EXPECT_NE(dump_doc(again).find("|       \"\xE6\x97\xA5\"\n|   <body>"), std::string::npos) << dump_doc(again);
  gtext_markup_document_free(again);
  // a UTF-16 request declares UTF-8
  o.encoding = "utf-16le";
  Collect c2;
  GTEXT_Markup_Sink sink2 = c2.sink();
  ASSERT_EQ(gtext_html_serialize(gtext_markup_document_root(doc), GTEXT_HTML_SERIALIZE_CHILDREN, &o, &sink2, &r), GTEXT_HTML_OK);
  EXPECT_NE(c2.bytes.find("<meta charset=\"utf-8\">"), std::string::npos);
  gtext_markup_document_free(doc);
}

namespace {

struct Doc {
  const char * label;
  const char * payload;
  bool bom;
};

} // namespace

TEST(HtmlSerializeEncoding, ParseWriteParseGivesOneTree) {
  const Doc docs[] = {
      {"utf-8", "\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80", false},
      {"windows-1252", "\xC3\xA9\xC3\xBC\xE2\x82\xAC", false},
      {"iso-8859-2", "\xC4\x9B\xC5\xA1", false},
      {"shift_jis", "\xE6\x97\xA5\xE6\x9C\xAC\xE3\x81\x82", false},
      {"euc-jp", "\xE6\x97\xA5\xE6\x9C\xAC\xE3\x81\x82", false},
      {"iso-2022-jp", "\xE6\x97\xA5\xE6\x9C\xAC\xE3\x81\x82", false},
      {"gbk", "\xE4\xB8\xAD\xE6\x96\x87", false},
      {"big5", "\xE4\xB8\xAD\xE6\x96\x87", false},
      {"euc-kr", "\xED\x95\x9C\xEA\xB5\xAD", false},
  };
  for (const Doc & d : docs) {
    SCOPED_TRACE(d.label);
    const std::string p = d.payload;
    const std::string utf8 = "<!DOCTYPE html><html><head><title>" + p + "</title></head><body><p class=\"" + p + "\">" + p
        + "&amp; &lt; &nbsp; <b>" + p + "</b></p><!-- c --><table><tr><td>" + p + "</td></tr></table></body></html>";
    // the encoded document
    const GTEXT_Encoding enc = gtext_encoding_for_label(d.label, std::strlen(d.label));
    char * bytes = nullptr;
    size_t n = 0;
    GTEXT_Encoder_Options eo = gtext_encoder_options_default();
    ASSERT_EQ(gtext_encoding_encode_buffer(enc, &eo, utf8.data(), utf8.size(), &bytes, &n, nullptr), GTEXT_ENCODING_OK);
    const std::string in(bytes, n);
    gtext_encoding_buffer_free(bytes);
    GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
    po.transport_encoding = d.label;
    GTEXT_Markup_Document * first = nullptr;
    ASSERT_EQ(gtext_html_parse(in.data(), in.size(), &po, &first, nullptr), GTEXT_HTML_OK);
    // written in the same encoding
    GTEXT_HTML_Serialize_Options so = gtext_html_serialize_options_default();
    so.encoding = d.label;
    Collect c;
    GTEXT_Markup_Sink sink = c.sink();
    GTEXT_HTML_Serialize_Result r;
    ASSERT_EQ(gtext_html_serialize(gtext_markup_document_root(first), GTEXT_HTML_SERIALIZE_CHILDREN, &so, &sink, &r), GTEXT_HTML_OK);
    EXPECT_EQ(r.references_written, 0u) << "everything was representable";
    GTEXT_Markup_Document * second = nullptr;
    ASSERT_EQ(gtext_html_parse(c.bytes.data(), c.bytes.size(), &po, &second, nullptr), GTEXT_HTML_OK);
    EXPECT_EQ(dump_doc(second), dump_doc(first));
    // and the bytes themselves are stable: writing the second again gives the same bytes
    Collect c2;
    GTEXT_Markup_Sink sink2 = c2.sink();
    ASSERT_EQ(gtext_html_serialize(gtext_markup_document_root(second), GTEXT_HTML_SERIALIZE_CHILDREN, &so, &sink2, nullptr), GTEXT_HTML_OK);
    EXPECT_EQ(hex(c2.bytes), hex(c.bytes));
    gtext_markup_document_free(first);
    gtext_markup_document_free(second);
  }
}

TEST(HtmlSerializeEncoding, ReferencesRoundTripToTheCharacter) {
  // characters the encoding lacks become references and come back as the characters
  const std::string utf8 = "<p title=\"\xE2\x82\xAC\xE6\x97\xA5\">\xE2\x82\xAC\xE6\x97\xA5\xF0\x9F\x98\x80</p>";
  for (const char * label : {"iso-8859-2", "windows-1250", "koi8-r", "shift_jis", "euc-kr", "iso-2022-jp", "big5"}) {
    SCOPED_TRACE(label);
    Tree t(utf8);
    const Written w = write(t.frag, label);
    ASSERT_EQ(w.status, GTEXT_HTML_OK);
    GTEXT_HTML_Fragment_Context ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.local_name = "body";
    GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    // the bytes are in the encoding: decode them (a document parse would) and parse as a fragment
    GTEXT_Markup_Document * whole = nullptr;
    po.transport_encoding = label;
    ASSERT_EQ(gtext_html_parse(w.bytes.data(), w.bytes.size(), &po, &whole, nullptr), GTEXT_HTML_OK);
    ASSERT_EQ(gtext_html_parse_fragment(&ctx, utf8.data(), utf8.size(), nullptr, &doc, &frag, nullptr), GTEXT_HTML_OK);
    // the body of the parsed document has the fragment's children
    const GTEXT_Markup_Node * body = nullptr;
    for (const GTEXT_Markup_Node * n = gtext_markup_node_first_child(gtext_markup_node_first_child(gtext_markup_document_root(whole)));
         n; n = gtext_markup_node_next_sibling(n)) {
      if (gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "body")) {
        body = n;
      }
    }
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(dump_fragment(gtext_markup_node_first_child(body) ? body : body), dump_fragment(frag));
    gtext_markup_document_free(doc);
    gtext_markup_document_free(whole);
  }
}

TEST(HtmlSerializeEncoding, TheXmlWriterIsUnchangedByAnHtmlDocumentsEncoding) {
  // an HTML document read from UTF-16 with a mark records UTF-16LE; the XML writer's UTF-8 has no mark for that
  std::string in = "\xFF\xFE";
  for (unsigned char c : std::string("<html><body><p>")) {
    in += char(c);
    in += '\0';
  }
  in += std::string("\xE5\x65", 2);
  for (unsigned char c : std::string("</p></body></html>")) {
    in += char(c);
    in += '\0';
  }
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  std::memset(&r, 0, sizeof(r));
  ASSERT_EQ(gtext_html_parse(in.data(), in.size(), nullptr, &doc, &r), GTEXT_HTML_OK);
  EXPECT_STREQ(r.encoding, "UTF-16LE");
  gtext_html_result_free(&r);
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(doc, &source), "UTF-16LE");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
  char * xml = nullptr;
  size_t n = 0;
  GTEXT_XML_Write_Options wo = gtext_xml_write_options_default();
  wo.xhtml = true; // (an HTML tree is in the XHTML namespace; the XHTML mode writes it)
  GTEXT_XML_Write_Result wr;
  ASSERT_EQ(gtext_xml_write_buffer(doc, &wo, &xml, &n, &wr), GTEXT_XML_OK) << wr.message << " at " << wr.path;
  const std::string out(xml, n);
  EXPECT_NE(out.compare(0, 3, "\xEF\xBB\xBF"), 0) << "no byte order mark: the document was not UTF-8";
  EXPECT_NE(out.find("\xE6\x97\xA5"), std::string::npos) << out;
  gtext_xml_write_buffer_free(doc, &wo, xml);
  gtext_markup_document_free(doc);
}

TEST(HtmlSerializeEncoding, EveryAllocationCanFailAndNothingLeaks) {
  // The encoder is an allocation of the serializer's; so are the head's queue and the buffers of the entry points.
  Tree t("<p title=\"\xE6\x97\xA5\xE2\x82\xAC\">\xE6\x97\xA5\xE2\x82\xAC<b>x</b></p>");
  GTEXT_Markup_Document * doc = nullptr;
  const std::string html = "<html><head><title>\xE6\x97\xA5</title></head><body>\xE6\x97\xA5</body></html>";
  ASSERT_EQ(gtext_html_parse(html.data(), html.size(), nullptr, &doc, nullptr), GTEXT_HTML_OK);
  for (int form = 0; form < 2; form++) {
    for (const char * label : {"shift_jis", "iso-2022-jp", "windows-1252"}) {
      bool reached_the_end = false;
      size_t oom_runs = 0;
      for (size_t n = 1; n <= 300 && !reached_the_end; n++) {
        FaultAllocator fa;
        GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
        o.encoding = label;
        o.allocator = &fa.alloc;
        o.inject_meta_charset = form == 1;
        char * buf = nullptr;
        size_t len = 0;
        fa.arm(n);
        const GTEXT_HTML_Status st = gtext_html_serialize_buffer(form == 1 ? gtext_markup_document_root(doc) : t.frag,
            GTEXT_HTML_SERIALIZE_CHILDREN, &o, &buf, &len, nullptr);
        fa.disarm();
        if (!fa.failed) {
          // (a windows-1252 write of the kanji in a title is a reference; nowhere is one forbidden here)
          EXPECT_EQ(st, GTEXT_HTML_OK) << label << " " << n;
          reached_the_end = true;
        }
        else if (st != GTEXT_HTML_OK) {
          oom_runs++;
          EXPECT_EQ(st, GTEXT_HTML_E_OOM) << label << " " << n;
          EXPECT_EQ(buf, nullptr);
        }
        if (buf) {
          gtext_html_serialize_buffer_free(nullptr, &o, buf);
        }
        EXPECT_EQ(fa.bad_frees, 0u);
        EXPECT_TRUE(fa.live.empty()) << label << " " << n << ": " << fa.live.size() << " blocks leaked";
      }
      EXPECT_TRUE(reached_the_end) << label;
      EXPECT_GT(oom_runs, 2u) << label;
    }
  }
  gtext_markup_document_free(doc);
}

namespace {

// The character set an ISO-2022-JP prefix ends in: 0 ASCII, 1 Roman, 2 JIS X 0208 (the escape sequences are followed).
int iso2022jp_state(const std::string & s) {
  int state = 0;
  for (size_t i = 0; i + 2 < s.size(); i++) {
    if (s[i] == '\x1B') {
      if (s[i + 1] == '$' && (s[i + 2] == 'B' || s[i + 2] == '@')) {
        state = 2;
      }
      else if (s[i + 1] == '(' && s[i + 2] == 'B') {
        state = 0;
      }
      else if (s[i + 1] == '(' && s[i + 2] == 'J') {
        state = 1;
      }
    }
  }
  return state;
}

} // namespace

// A prefix that ends in the two-byte set would make what follows it (more of the document in the caller's stream, say) read as kanji.
TEST(HtmlSerializeEncoding, Iso2022JpPrefixAfterAFailureEndsInAscii) {
  // an unwritable character in a comment, after Japanese text and in the middle of some
  Tree t("<p>\xE6\x97\xA5\xE6\x9C\xAC</p><!--\xE6\x97\xA5\xE2\x82\xAC-->");
  const Written w = write(t.frag, "iso-2022-jp");
  ASSERT_EQ(w.status, GTEXT_HTML_E_UNREPRESENTABLE);
  EXPECT_EQ(w.result.unrepresentable_code_point, 0x20ACu);
  EXPECT_EQ(iso2022jp_state(w.bytes), 0) << hex(w.bytes);
  ASSERT_GE(w.bytes.size(), 3u);
  EXPECT_EQ(w.bytes.substr(w.bytes.size() - 3), "\x1B(B");
  EXPECT_EQ(w.bytes.substr(w.bytes.size() - 8, 5), "\x1B$BF|") << "the character before the failure is kept";

  // the output limit, at every size: never more than the limit, and in ASCII at the end unless there was no room for the three bytes
  Tree u("<p>\xE6\x97\xA5\xE6\x9C\xAC</p><p>a\xE6\x97\xA5" "b\xE6\x97\xA5\xE6\x9C\xAC</p>");
  const Written full = write(u.frag, "iso-2022-jp");
  ASSERT_EQ(full.status, GTEXT_HTML_OK);
  EXPECT_EQ(iso2022jp_state(full.bytes), 0);
  size_t closed = 0;
  for (size_t limit = 1; limit < full.bytes.size(); limit++) {
    const Written x = write(u.frag, "iso-2022-jp", false, limit);
    ASSERT_EQ(x.status, GTEXT_HTML_E_LIMIT_OUTPUT) << limit;
    ASSERT_LE(x.bytes.size(), limit) << "the closing escape is written only within the limit";
    if (iso2022jp_state(x.bytes) != 0) {
      EXPECT_LT(limit - x.bytes.size(), 3u) << limit << ": left in the two-byte set with room to leave it: " << hex(x.bytes);
    }
    else if (x.bytes.size() >= 3 && x.bytes.substr(x.bytes.size() - 3) == "\x1B(B") {
      closed++;
    }
  }
  EXPECT_GT(closed, 0u);

  // a sink that refuses: nothing more is written to it, and the status is the refusal
  Tree v("<p>\xE6\x97\xA5\xE6\x9C\xAC</p><p>\xE6\x97\xA5</p>");
  GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
  o.encoding = "iso-2022-jp";
  Collect c;
  c.refuse_after = 0;
  GTEXT_Markup_Sink sink = c.sink();
  GTEXT_HTML_Serialize_Result r;
  EXPECT_EQ(gtext_html_serialize(v.frag, GTEXT_HTML_SERIALIZE_CHILDREN, &o, &sink, &r), GTEXT_HTML_E_WRITE);
  EXPECT_EQ(c.calls, 0u);
}
