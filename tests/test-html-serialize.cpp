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
 * The HTML serializer: what it writes.
 *
 * What is claimed, and where it is checked:
 *
 * - Every row of the specification's matrix, and each rule of the standard's algorithm on its own: the escaping of
 *   text, of raw text, of attribute values; void elements, with children and without; templates; the serialized name of
 *   every kind of attribute; names as the tree has them; the line feed of pre, textarea and listing; every kind of
 *   node as the node to write, in both scopes.
 * - Each option, with the vector that defines it where there is one, and the combinations that are refused.
 * - The optional tags of the standard (13.1.2.4), by its own examples and by every rule on its own, through the tree walk
 *   and the token stream both.
 * - The token stream's arguments and states.
 *
 * What the vectors and the differential check (tools/conformance/html_serializer_suite.py, tools/oracle/
 * html_serialize_diff.py) is not repeated here; what the other file checks (test-html-serialize-limits.cpp) is the
 * sink and the allocator failing, the limits, depth and the work.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>

#include "html_serialize_test_support.h"

// The serializer's own table of elements is searched by bisection, so it must stay in strcmp order (a hidden function, reached
// through the static archive the tests link).
extern "C" bool html_serialize_table_sorted(void);

namespace {

using namespace hs;

TEST(HtmlSerialize, TheTableOfElementsTheRulesNameIsInOrder) {
  EXPECT_TRUE(html_serialize_table_sorted());
}

constexpr GTEXT_HTML_Serialize_Scope CHILDREN = GTEXT_HTML_SERIALIZE_CHILDREN;
constexpr GTEXT_HTML_Serialize_Scope OUTER = GTEXT_HTML_SERIALIZE_OUTER;

// ---- the defaults ---------------------------------------------------------------------------------

TEST(HtmlSerialize, TheDefaultsAreSpelledOutAndZeroIsTheDefault) {
  const Opts d = defaults();
  EXPECT_EQ(d.allocator, nullptr);
  EXPECT_EQ(d.max_depth, 1024u);
  EXPECT_EQ(d.max_output_bytes, 256u * 1024 * 1024);
  EXPECT_FALSE(d.scripting);
  EXPECT_EQ(d.encoding, nullptr);
  EXPECT_EQ(d.quote_attr_values, GTEXT_HTML_QUOTE_ALWAYS);
  EXPECT_EQ(d.quote_char, 0);
  EXPECT_FALSE(d.use_best_quote_char);
  EXPECT_FALSE(d.omit_optional_tags);
  EXPECT_FALSE(d.minimize_boolean_attributes);
  EXPECT_FALSE(d.use_trailing_solidus);
  EXPECT_FALSE(d.space_before_trailing_solidus);
  EXPECT_FALSE(d.escape_rcdata);
  EXPECT_FALSE(d.strip_whitespace);
  EXPECT_FALSE(d.inject_meta_charset);
  EXPECT_FALSE(d.alphabetical_attributes);
  EXPECT_FALSE(d.doctype_identifiers);

  Opts zero;
  std::memset(&zero, 0, sizeof(zero));
  Parsed p = parse_fragment("<p class=a title='x\"y'>t&amp;<br>u</p>");
  EXPECT_EQ(children(p.top(), zero), children(p.top(), d));
  EXPECT_EQ(children(p.top(), zero), "<p class=\"a\" title=\"x&quot;y\">t&amp;<br>u</p>");
  // NULL options are the defaults too.
  char * buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_html_serialize_buffer(p.top(), CHILDREN, nullptr, &buf, &len, nullptr), GTEXT_HTML_OK);
  EXPECT_EQ(std::string(buf, len), children(p.top(), d));
  gtext_html_serialize_buffer_free(p.doc, nullptr, buf);
}

// ---- the matrix, row by row -------------------------------------------------------------------------

TEST(HtmlSerialize, TextIsEscapedForAmpersandNoBreakSpaceAndAngleBrackets) {
  Parsed p = parse_fragment("<p>a&lt;b&amp;c&nbsp;d&gt;e\"f'g</p>");
  EXPECT_EQ(outer(find(p.top(), "p")), "<p>a&lt;b&amp;c&nbsp;d&gt;e\"f'g</p>");
  // A U+00A0 is two bytes of UTF-8 and nothing else is rewritten, including other no-break characters.
  Build b;
  Node * d = b.el(nullptr, "div");
  b.text(d, "x\xC2\xA0y\xC2\xA1z \xE2\x80\xAF \xC3\x82");
  EXPECT_EQ(children(d), "x&nbsp;y\xC2\xA1z \xE2\x80\xAF \xC3\x82");
}

TEST(HtmlSerialize, TextInARawTextParentIsWrittenAsItIs) {
  const char * const raw[] = {"style", "script", "xmp", "iframe", "noembed", "noframes", "plaintext"};
  for (const char * name : raw) {
    SCOPED_TRACE(name);
    Build b;
    Node * e = b.el(nullptr, name);
    b.text(e, "a<b&c>d\xC2\xA0\"");
    EXPECT_EQ(children(e), "a<b&c>d\xC2\xA0\"");
    EXPECT_EQ(outer(e), std::string("<") + name + ">a<b&c>d\xC2\xA0\"</" + name + ">");
  }
  Parsed p = parse_fragment("<script>a<b</script>");
  EXPECT_EQ(children(p.top()), "<script>a<b</script>");
}

TEST(HtmlSerialize, TheRawTextListIsTheHtmlNamespaceOnly) {
  // The same names in another namespace, or in none, are ordinary elements.
  for (const char * ns : {GTEXT_MARKUP_NS_SVG, GTEXT_MARKUP_NS_MATHML, static_cast<const char *>(nullptr)}) {
    Build b;
    Node * e = b.el(nullptr, "style", ns);
    b.text(e, "a<b");
    EXPECT_EQ(children(e), "a&lt;b");
  }
  // ... and so is a raw text element's name with another case.
  Build b;
  Node * e = b.el(nullptr, "SCRIPT");
  b.text(e, "a<b");
  EXPECT_EQ(children(e), "a&lt;b");
}

TEST(HtmlSerialize, NoscriptIsRawTextOnlyWithTheScriptingFlag) {
  Parsed on = parse_fragment("<noscript><p>a&lt;</p></noscript>", "div", true);
  Node * n = find(on.top(), "noscript");
  Opts scripting = defaults();
  scripting.scripting = true;
  // Parsed with scripting, the content is one text node; written with the flag it comes out as it came in.
  EXPECT_EQ(outer(n, scripting), "<noscript><p>a&lt;</p></noscript>");
  EXPECT_EQ(outer(n), "<noscript>&lt;p&gt;a&amp;lt;&lt;/p&gt;</noscript>");
  // Parsed without, it is elements, and the flag changes only text that is a child of noscript.
  Parsed off = parse_fragment("<noscript><p>a&lt;</p></noscript>", "div", false);
  EXPECT_EQ(outer(find(off.top(), "noscript")), "<noscript><p>a&lt;</p></noscript>");
  EXPECT_EQ(outer(find(off.top(), "noscript"), scripting), "<noscript><p>a&lt;</p></noscript>");
}

TEST(HtmlSerialize, AttributeValuesAreAlwaysDoubleQuotedAndEscaped) {
  Parsed p = parse_fragment("<p title='a\"b&amp;' lang=en data-x=\"&lt;&gt;&nbsp;'\" empty hidden=\"\">");
  EXPECT_EQ(outer(find(p.top(), "p")),
      "<p title=\"a&quot;b&amp;\" lang=\"en\" data-x=\"&lt;&gt;&nbsp;'\" empty=\"\" hidden=\"\"></p>");
}

TEST(HtmlSerialize, VoidElementsHaveNoEndTagAndNoContent) {
  Parsed p = parse_fragment("<br><img src=x><input disabled><hr/><wbr><embed><area><base><link><meta><source><track>");
  EXPECT_EQ(children(p.top()),
      "<br><img src=\"x\"><input disabled=\"\"><hr><wbr><embed><area><base><link><meta><source><track>");
  // The legacy ones the parser and serializer still treat as void.
  Build b;
  Node * d = b.el(b.el(nullptr, "html"), "div");
  for (const char * name : {"basefont", "bgsound", "frame", "keygen", "param"}) {
    b.el(d, name);
  }
  EXPECT_EQ(children(d), "<basefont><bgsound><frame><keygen><param>");
}

TEST(HtmlSerialize, AVoidElementWithChildrenIsWrittenAsTheStandardSaysAndTheChildrenAreNotWritten) {
  Build b;
  Node * html = b.el(nullptr, "html");
  Node * br = b.el(html, "br");
  b.text(br, "text");
  Node * img = b.el(html, "img");
  b.attr(img, "src", "s");
  Node * inner = b.el(img, "p");
  b.text(inner, "x");
  b.text(html, "after");
  EXPECT_EQ(children(html), "<br><img src=\"s\">after");
  EXPECT_EQ(outer(br), "<br>");
  EXPECT_EQ(outer(img), "<img src=\"s\">");
  // The children of a void element: "If the node serializes as void, then return the empty string".
  EXPECT_EQ(children(br), "");
  EXPECT_EQ(children(img), "");
  // A name that only looks void is not: another case, another namespace.
  Node * big = b.el(html, "BR");
  b.text(big, "t");
  Node * svg = b.el(html, "br", GTEXT_MARKUP_NS_SVG);
  b.text(svg, "t");
  EXPECT_EQ(outer(big), "<BR>t</BR>");
  EXPECT_EQ(outer(svg), "<br>t</br>");
}

TEST(HtmlSerialize, ATemplateWritesItsContentsInPlaceOfItsChildren) {
  Parsed p = parse_fragment("<template><p>x</template>after");
  Node * t = find(p.top(), "template");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(children(p.top()), "<template><p>x</p></template>after");
  EXPECT_EQ(outer(t), "<template><p>x</p></template>");
  EXPECT_EQ(children(t), "<p>x</p>");
  // A template with children of its own (the tree allows it) writes the contents only.
  Build b;
  Node * top = b.fragment();
  Node * tpl = b.el(top, "template");
  b.el(tpl, "span");
  Node * content = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(tpl, &content), GTEXT_MARKUP_OK);
  Node * i = b.make("i");
  ASSERT_EQ(gtext_markup_append_child(content, i), GTEXT_MARKUP_OK);
  EXPECT_EQ(outer(tpl), "<template><i></i></template>");
  // ... and one with no contents at all writes none.
  Node * bare = b.el(top, "template");
  b.el(bare, "span");
  EXPECT_EQ(outer(bare), "<template></template>");
  // A template inside a template.
  Parsed q = parse_fragment("<template><template><b>y</b></template></template>");
  EXPECT_EQ(children(q.top()), "<template><template><b>y</b></template></template>");
}

TEST(HtmlSerialize, TheSerializedNameOfEveryKindOfAttribute) {
  Build b;
  Node * e = b.el(nullptr, "svg", GTEXT_MARKUP_NS_SVG);
  b.attr(e, "plain", "1");
  b.attr(e, "lang", "en", GTEXT_MARKUP_NS_XML, "xml");
  b.attr(e, "space", "p", GTEXT_MARKUP_NS_XML, nullptr);          // the prefix is "xml:" whatever the tree says
  b.attr(e, "xmlns", "u", GTEXT_MARKUP_NS_XMLNS, nullptr);
  b.attr(e, "xlink", "u2", GTEXT_MARKUP_NS_XMLNS, "xmlns");
  b.attr(e, "href", "h", GTEXT_MARKUP_NS_XLINK, "xlink");
  b.attr(e, "title", "t", GTEXT_MARKUP_NS_XLINK, "other");        // "xlink:" whatever the prefix is
  b.attr(e, "a", "1", "urn:other", "o");                          // some other namespace: the qualified name
  b.attr(e, "b", "2", "urn:other", nullptr);                      // ... with no prefix: the local name
  EXPECT_EQ(outer(e),
      "<svg plain=\"1\" xml:lang=\"en\" xml:space=\"p\" xmlns=\"u\" xmlns:xlink=\"u2\" xlink:href=\"h\" xlink:title=\"t\" "
      "o:a=\"1\" b=\"2\"></svg>");
  // Through the parser: the nine attributes that HTML reads as namespaced.
  Parsed p = parse_fragment("<svg xmlns:xlink=\"http://www.w3.org/1999/xlink\"><a xlink:href=\"u\" xml:lang=\"en\" "
                            "viewBox=\"0 0 1 1\"></a></svg>");
  EXPECT_EQ(children(p.top()),
      "<svg xmlns:xlink=\"http://www.w3.org/1999/xlink\"><a xlink:href=\"u\" xml:lang=\"en\" viewBox=\"0 0 1 1\"></a></svg>");
}

TEST(HtmlSerialize, ElementNamesAreTheLocalNameInTheThreeHtmlNamespacesAndTheQualifiedNameElsewhere) {
  Build b;
  Node * d = b.el(nullptr, "div");
  b.el(d, "p", GTEXT_MARKUP_NS_HTML, "h");                 // prefixed, in the HTML namespace: the local name
  b.el(d, "path", GTEXT_MARKUP_NS_SVG, "s");
  b.el(d, "mi", GTEXT_MARKUP_NS_MATHML, "m");
  b.el(d, "x", "urn:other", "o");                          // the qualified name
  b.el(d, "y", "urn:other", nullptr);
  b.el(d, "z", nullptr, nullptr);                          // no namespace
  EXPECT_EQ(children(d), "<p></p><path></path><mi></mi><o:x></o:x><y></y><z></z>");
}

TEST(HtmlSerialize, PreTextareaAndListingGetNoExtraLineFeedAndTheirOwnIsKept) {
  // "this algorithm does not serialize an extra U+000A (LF) character": a text that starts with one is written
  // with one, which the parser then drops when it reads it back.
  Parsed p = parse_fragment("<pre>\n\nHello.</pre><textarea>\n\nx</textarea><listing>\n\ny</listing><pre>\nz</pre>");
  EXPECT_EQ(children(p.top()), "<pre>\nHello.</pre><textarea>\nx</textarea><listing>\ny</listing><pre>z</pre>");
  Build b;
  Node * pre = b.el(nullptr, "pre");
  b.text(pre, "\nx");
  EXPECT_EQ(outer(pre), "<pre>\nx</pre>");
  Parsed again = parse_fragment(outer(pre));
  EXPECT_EQ(children(again.top()), "<pre>x</pre>"); // the known loss, as the standard documents it
}

TEST(HtmlSerialize, CommentsProcessingInstructionsAndDocumentTypes) {
  Parsed p = parse_document("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://x/y.dtd\"><!--c -- d--><html>");
  EXPECT_EQ(children(p.top()), "<!DOCTYPE html><!--c -- d--><html><head></head><body></body></html>");
  Build b;
  Node * d = b.el(nullptr, "div");
  b.comment(d, "a-->b");                           // the standard writes it, and says it will not read back
  b.pi(d, "target", "some data");
  b.pi(d, "t", "");
  b.cdata(d, "<c>&");                              // a CDATA section is a text node
  EXPECT_EQ(children(d), "<!--a-->b--><?target some data?><?t ?>&lt;c&gt;&amp;");
  // A document type with no name, and one with every identifier: the name only.
  Build c;
  c.doctype("", nullptr, nullptr);
  EXPECT_EQ(children(c.root()), "<!DOCTYPE >");
  Build e;
  e.doctype("html", "pub", "sys");
  EXPECT_EQ(children(e.root()), "<!DOCTYPE html>");
}

TEST(HtmlSerialize, EveryKindOfNodeIsAScopeOfItsOwn) {
  Parsed p = parse_document("<!DOCTYPE html><!--c--><p class=a>t&amp;</p>");
  Node * html = find(p.top(), "html");
  ASSERT_NE(html, nullptr);
  Node * body = find(p.top(), "body");
  Node * para = find(p.top(), "p");
  Node * text = gtext_markup_node_first_child(para);
  Node * comment = gtext_markup_node_next_sibling(gtext_markup_node_first_child(p.top()));
  Node * doctype = gtext_markup_node_first_child(p.top());
  ASSERT_EQ(gtext_markup_node_kind(text), GTEXT_MARKUP_KIND_TEXT);
  ASSERT_EQ(gtext_markup_node_kind(comment), GTEXT_MARKUP_KIND_COMMENT);
  ASSERT_EQ(gtext_markup_node_kind(doctype), GTEXT_MARKUP_KIND_DOCTYPE);
  EXPECT_EQ(outer(p.top()), children(p.top()));                  // a document: its children
  EXPECT_EQ(outer(html), "<html><head></head><body><p class=\"a\">t&amp;</p></body></html>");
  EXPECT_EQ(children(body), "<p class=\"a\">t&amp;</p>");
  EXPECT_EQ(outer(para), "<p class=\"a\">t&amp;</p>");
  EXPECT_EQ(outer(text), "t&amp;");
  EXPECT_EQ(outer(comment), "<!--c-->");
  EXPECT_EQ(outer(doctype), "<!DOCTYPE html>");
  // A leaf has no children: asking for them is a refusal, not an empty answer.
  for (Node * leaf : {text, comment, doctype}) {
    GTEXT_HTML_Serialize_Result r;
    char * buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(gtext_html_serialize_buffer(leaf, GTEXT_HTML_SERIALIZE_CHILDREN, nullptr, &buf, &len, &r),
              GTEXT_HTML_E_INVALID);
    EXPECT_EQ(buf, nullptr);
  }
  // A text node written alone is escaped for its parent, which is what the walk would do.
  Parsed s = parse_fragment("<script>a<b</script><style>x>y</style><p>a&lt;b</p>");
  EXPECT_EQ(outer(gtext_markup_node_first_child(find(s.top(), "script"))), "a<b");
  EXPECT_EQ(outer(gtext_markup_node_first_child(find(s.top(), "style"))), "x>y");
  EXPECT_EQ(outer(gtext_markup_node_first_child(find(s.top(), "p"))), "a&lt;b");
  // A fragment: its children.
  Build b;
  Node * f = b.fragment();
  Node * x = b.make("b");
  ASSERT_EQ(gtext_markup_append_child(f, x), GTEXT_MARKUP_OK);
  b.text(f, "t");
  EXPECT_EQ(children(f), "<b></b>t");
  EXPECT_EQ(outer(f), "<b></b>t");
  // A fragment's children are the contents of nothing: raw text needs the context of an element.
  Build c;
  Node * script = c.el(nullptr, "script");
  c.text(script, "<x>");
  EXPECT_EQ(children(script), "<x>");
  EXPECT_EQ(outer(script), "<script><x></script>");
}

TEST(HtmlSerialize, TheChildrenOfAnElementAreTheContentsOfItsContextForRawText) {
  // `innerHTML` of a style element writes its text raw; the tree walk knows the parent of what it writes.
  Parsed p = parse_fragment("a<b&c", "style");
  Build b;
  Node * style = b.el(nullptr, "style");
  for (Node * c = gtext_markup_node_first_child(p.fragment); c; c = gtext_markup_node_next_sibling(c)) {
    Node * copy = nullptr;
    ASSERT_EQ(gtext_markup_clone(b.doc, c, true, &copy), GTEXT_MARKUP_OK);
    ASSERT_EQ(gtext_markup_append_child(style, copy), GTEXT_MARKUP_OK);
  }
  EXPECT_EQ(children(style), "a<b&c");
  EXPECT_EQ(children(p.fragment), "a&lt;b&amp;c"); // the fragment node itself has no parent element
}

// ---- options --------------------------------------------------------------------------------------

struct QuoteCase {
  const char * value;
  const char * when_needed;
};

TEST(HtmlSerializeOptions, QuoteWhenNeededIsDefinedByTheVectorsAndEscapesAsAlways) {
  Opts o = defaults();
  o.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
  const QuoteCase cases[] = {
      {"foo", "title=foo"},
      {"", "title=\"\""},
      {"foo bar", "title=\"foo bar\""},
      {"foo\tbar", "title=\"foo\tbar\""},
      {"foo\nbar", "title=\"foo\nbar\""},
      {"foo\rbar", "title=\"foo\rbar\""},
      {"foo\fbar", "title=\"foo\fbar\""},
      {"foo\vbar", "title=foo\vbar"},                    // core.test "proper attribute value non-quoting (with linetab)"
      {"foo<bar", "title=foo&lt;bar"},                    // ... "(with <)"
      {"foo>bar", "title=\"foo&gt;bar\""},                // ... "proper attribute value quoting (with >)"
      {"foo=bar", "title=\"foo=bar\""},
      {"foo`bar", "title=\"foo`bar\""},
      {"foo\"bar", "title=\"foo&quot;bar\""},
      {"foo'bar", "title=\"foo'bar\""},
      {"foo&bar", "title=foo&amp;bar"},
      {"a\xC2\xA0z", "title=a&nbsp;z"},
      {"foo/bar", "title=foo/bar"},
  };
  for (const QuoteCase & c : cases) {
    SCOPED_TRACE(c.value);
    Build b;
    Node * e = b.el(nullptr, "span");
    b.attr(e, "title", c.value);
    EXPECT_EQ(outer(e, o), std::string("<span ") + c.when_needed + "></span>");
    // Always quoting is the same value in quotes, and the same escapes.
    const std::string always = outer(e);
    EXPECT_EQ(always.rfind("<span title=\"", 0), 0u);
  }
}

TEST(HtmlSerializeOptions, TheQuoteCharacterAndTheBestOne) {
  Build b;
  Node * e = b.el(nullptr, "span");
  b.attr(e, "title", "test 'with' quote_char");
  Opts o = defaults();
  o.quote_char = '\'';
  EXPECT_EQ(outer(e, o), "<span title='test &#39;with&#39; quote_char'></span>"); // options.test "quote_char"
  o.quote_char = '"';
  EXPECT_EQ(outer(e, o), "<span title=\"test 'with' quote_char\"></span>");

  Opts best = defaults();
  best.use_best_quote_char = true;
  struct {
    const char * value;
    const char * out;
  } cases[] = {
      {"plain", "<span title=\"plain\"></span>"},
      {"it's", "<span title=\"it's\"></span>"},
      {"say \"x\"", "<span title='say \"x\"'></span>"},      // core.test "proper attribute value quoting (with \")"
      {"a'b\"c", "<span title=\"a'b&quot;c\"></span>"},      // both: the default quote, escaped
  };
  for (auto & c : cases) {
    Build d;
    Node * s = d.el(nullptr, "span");
    d.attr(s, "title", c.value);
    EXPECT_EQ(outer(s, best), c.out) << c.value;
  }
  // When needed and best together: unquoted when nothing needs quotes.
  best.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
  Build d;
  Node * s = d.el(nullptr, "span");
  d.attr(s, "title", "say \"x\" 'y'");
  EXPECT_EQ(outer(s, best), "<span title=\"say &quot;x&quot; 'y'\"></span>");
}

TEST(HtmlSerializeOptions, MinimizeBooleanAttributes) {
  Opts o = defaults();
  o.minimize_boolean_attributes = true;
  struct {
    const char * html;
    const char * out;
  } cases[] = {
      {"<button disabled=disabled>", "<button disabled></button>"},       // options.test "quote_attr_values=true"
      {"<button disabled>", "<button disabled></button>"},
      {"<button disabled=DISABLED>", "<button disabled></button>"},
      {"<button disabled=other>", "<button disabled=\"other\"></button>"}, // the value would be lost
      {"<button class=\"\">", "<button class=\"\"></button>"},              // not a boolean attribute
      {"<p disabled>", "<p disabled=\"\"></p>"},                           // not on a p
      {"<input checked readonly>", "<input checked readonly>"},
      {"<div autofocus inert>", "<div autofocus inert></div>"},            // the ones the standard lists for every element
  };
  for (auto & c : cases) {
    SCOPED_TRACE(c.html);
    Parsed p = parse_fragment(c.html);
    Out out = serialize_with(p.top(), CHILDREN, o);
    EXPECT_EQ(out.bytes, c.out);
  }
  Parsed p = parse_fragment("<input checked readonly><button disabled=disabled>");
  Out out = serialize_with(p.top(), CHILDREN, o);
  EXPECT_EQ(out.r.minimized_attributes, 3u);
  // ... which a quoting option does not undo.
  o.quote_attr_values = GTEXT_HTML_QUOTE_ALWAYS;
  EXPECT_EQ(serialize_with(p.top(), CHILDREN, o).bytes, "<input checked readonly><button disabled></button>");
  // An attribute in a namespace is never one, nor is an element outside HTML.
  Build b;
  Node * e = b.el(nullptr, "button", GTEXT_MARKUP_NS_SVG);
  b.attr(e, "disabled", "");
  EXPECT_EQ(outer(e, o), "<button disabled=\"\"></button>");
}

TEST(HtmlSerializeOptions, TrailingSolidus) {
  Opts o = defaults();
  o.use_trailing_solidus = true;
  Parsed p = parse_fragment("<img><div><br class=a></div>");
  EXPECT_EQ(children(p.top(), o), "<img/><div><br class=\"a\"/></div>");     // options.test: only void elements
  o.space_before_trailing_solidus = true;
  EXPECT_EQ(children(p.top(), o), "<img /><div><br class=\"a\" /></div>");    // "use_trailing_solidus=true with void element"
  // Elements that look void and are not are not given one.
  Build b;
  Node * s = b.el(nullptr, "br", GTEXT_MARKUP_NS_SVG);
  EXPECT_EQ(outer(s, o), "<br></br>");
  // A solidus right after an unquoted value is part of the value (`<br class=a/>` has the class `a/`), so with no space
  // before it the last attribute of a void element is quoted whatever the quoting mode says.
  Opts q = defaults();
  q.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
  q.use_trailing_solidus = true;
  Parsed p2 = parse_fragment("<br class=a id=b><div class=a id=b></div><img src=x>");
  EXPECT_EQ(children(p2.top(), q), "<br class=a id=\"b\"/><div class=a id=b></div><img src=\"x\"/>");
  q.space_before_trailing_solidus = true;
  EXPECT_EQ(children(p2.top(), q), "<br class=a id=b /><div class=a id=b></div><img src=x />");
  // The round trip is the reason: what was written reads back as the same tree.
  q.space_before_trailing_solidus = false;
  Parsed back = parse_fragment(children(p2.top(), q));
  EXPECT_TRUE(gtext_markup_equal(p2.top(), back.top()));
}

TEST(HtmlSerializeOptions, EscapeRawText) {
  Opts o = defaults();
  o.escape_rcdata = true;
  Build b;
  Node * top = b.fragment();
  Node * s = b.el(top, "script");
  b.text(s, "a<b>c&d");
  EXPECT_EQ(outer(s, o), "<script>a&lt;b&gt;c&amp;d</script>");  // options.test "rcdata"
  Node * x = b.el(top, "plaintext");
  b.text(x, "<");
  EXPECT_EQ(outer(x, o), "<plaintext>&lt;</plaintext>");
}

TEST(HtmlSerializeOptions, StripWhitespace) {
  Opts o = defaults();
  o.strip_whitespace = true;
  struct {
    const char * html;
    const char * out;
  } cases[] = {
      {"\t\r\n\f foo", " foo"},                                   // whitespace.test
      {"foo \t\r\n\f", "foo "},
      {"foo \t\r\n\f bar", "foo bar"},
      {"<pre>\t \n foo \n bar \t</pre>", "<pre>\t \n foo \n bar \t</pre>"},
      {"<pre> a<span> b  c </span> d </pre>", "<pre> a<span> b  c </span> d </pre>"},   // below a pre too
      {"<textarea> a  b </textarea>", "<textarea> a  b </textarea>"},
      {"<script> a  b </script><style> c  d </style>", "<script> a  b </script><style> c  d </style>"},
      {"<p> a  <b> b  </b>  c </p>", "<p> a <b> b </b> c </p>"},
      {"<p>a&nbsp;&nbsp;b</p>", "<p>a&nbsp;&nbsp;b</p>"},        // a no-break space is not white space
      {"<xmp>  a  </xmp><plaintext>  b  ", "<xmp>  a  </xmp><plaintext>  b  </plaintext>"},
  };
  for (auto & c : cases) {
    SCOPED_TRACE(c.html);
    Parsed p = parse_fragment(c.html);
    EXPECT_EQ(children(p.top(), o), c.out);
  }
  // Two text nodes in a row are one run (the output does not depend on how the text was cut).
  Build b;
  Node * d = b.el(nullptr, "div");
  b.text(d, "a  ");
  b.text(d, "  b");
  b.text(d, " ");
  b.text(d, "c");
  EXPECT_EQ(children(d, o), "a b c");
  o.escape_rcdata = true;  // the text of a script is still left alone: the elements are what is remembered
  Node * s = b.el(b.fragment(), "script");
  b.text(s, "a  <  b");
  EXPECT_EQ(children(s, o), "a  &lt;  b");
}

TEST(HtmlSerializeOptions, AlphabeticalAttributes) {
  Opts o = defaults();
  o.alphabetical_attributes = true;
  Build b;
  Node * e = b.el(nullptr, "meta");
  b.attr(e, "name", "robots");
  b.attr(e, "content", "noindex");
  b.attr(e, "href", "h", GTEXT_MARKUP_NS_XLINK, "xlink");
  b.attr(e, "b", "1");
  b.attr(e, "a", "2");
  // By namespace (none first), then name.
  EXPECT_EQ(outer(e, o), "<meta a=\"2\" b=\"1\" content=\"noindex\" name=\"robots\" xlink:href=\"h\">");
  EXPECT_EQ(outer(e), "<meta name=\"robots\" content=\"noindex\" xlink:href=\"h\" b=\"1\" a=\"2\">");
}

TEST(HtmlSerializeOptions, DoctypeIdentifiers) {
  Opts o = defaults();
  o.doctype_identifiers = true;
  struct {
    const char * name;
    const char * pub;
    const char * sys;
    const char * out;
  } cases[] = {
      {"HTML", nullptr, nullptr, "<!DOCTYPE HTML>"},
      {"HTML", "-//W3C//DTD HTML 4.01//EN", "http://www.w3.org/TR/html4/strict.dtd",
          "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://www.w3.org/TR/html4/strict.dtd\">"},
      {"HTML", "-//W3C//DTD HTML 4.01//EN", nullptr, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\">"},
      {"html", "", "http://www.ibm.com/x.dtd", "<!DOCTYPE html SYSTEM \"http://www.ibm.com/x.dtd\">"},
      {"html", nullptr, "a\"b", "<!DOCTYPE html SYSTEM 'a\"b'>"},
      {"html", "", "", "<!DOCTYPE html>"},
  };
  for (auto & c : cases) {
    Build b;
    b.doctype(c.name, c.pub, c.sys);
    EXPECT_EQ(children(b.root(), o), c.out);
  }
}

TEST(HtmlSerializeOptions, TheCombinationsThatMeanNothingAreRefusedBeforeAnythingIsWritten) {
  Parsed p = parse_fragment("<p>x</p>");
  struct {
    const char * what;
    std::function<void(Opts &)> set;
    GTEXT_HTML_Status status;
  } cases[] = {
      {"space without solidus", [](Opts & o) { o.space_before_trailing_solidus = true; }, GTEXT_HTML_E_OPTIONS},
      {"best with a fixed quote", [](Opts & o) { o.use_best_quote_char = true; o.quote_char = '\''; }, GTEXT_HTML_E_OPTIONS},
      {"best with the default quote written out", [](Opts & o) { o.use_best_quote_char = true; o.quote_char = '"'; },
          GTEXT_HTML_E_OPTIONS},
      {"a quote that is no quote", [](Opts & o) { o.quote_char = '`'; }, GTEXT_HTML_E_OPTIONS},
      {"another quote that is no quote", [](Opts & o) { o.quote_char = 'x'; }, GTEXT_HTML_E_OPTIONS},
      {"a quoting mode out of range", [](Opts & o) { o.quote_attr_values = (GTEXT_HTML_Quote_Mode)7; }, GTEXT_HTML_E_OPTIONS},
      {"a negative quoting mode", [](Opts & o) { o.quote_attr_values = (GTEXT_HTML_Quote_Mode)-1; }, GTEXT_HTML_E_OPTIONS},
      {"a label for nothing", [](Opts & o) { o.encoding = "no-such-encoding"; }, GTEXT_HTML_E_UNSUPPORTED_ENCODING},
      {"utf-7 is not a label", [](Opts & o) { o.encoding = "utf-7"; }, GTEXT_HTML_E_UNSUPPORTED_ENCODING},
      {"the empty label", [](Opts & o) { o.encoding = ""; }, GTEXT_HTML_E_UNSUPPORTED_ENCODING},
      {"a label that is almost utf-8", [](Opts & o) { o.encoding = "utf-8x"; }, GTEXT_HTML_E_UNSUPPORTED_ENCODING},
  };
  for (auto & c : cases) {
    SCOPED_TRACE(c.what);
    Opts o = defaults();
    c.set(o);
    Collect sink;
    GTEXT_Markup_Sink s = sink.sink();
    GTEXT_HTML_Serialize_Result r;
    EXPECT_EQ(gtext_html_serialize(p.top(), CHILDREN, &o, &s, &r), c.status);
    EXPECT_EQ(r.status, c.status);
    EXPECT_EQ(sink.calls, 0u);
    char * buf = nullptr;
    size_t len = 7;
    EXPECT_EQ(gtext_html_serialize_buffer(p.top(), CHILDREN, &o, &buf, &len, nullptr), c.status);
    EXPECT_EQ(buf, nullptr);
    EXPECT_EQ(len, 7u);
    GTEXT_HTML_Serializer * ser = reinterpret_cast<GTEXT_HTML_Serializer *>(0x1);
    EXPECT_EQ(gtext_html_serializer_new(&o, &s, &ser), c.status);
    EXPECT_EQ(ser, nullptr);
  }
  // The statuses have names of their own.
  EXPECT_STREQ(gtext_html_status_string(GTEXT_HTML_E_OPTIONS), "unusable combination of options");
  EXPECT_STREQ(gtext_html_status_string(GTEXT_HTML_E_WRITE), "output refused");
  EXPECT_STREQ(gtext_html_status_string(GTEXT_HTML_E_LIMIT_OUTPUT), "output too long");
}

TEST(HtmlSerializeOptions, TheUtf8LabelsAreAccepted) {
  Parsed p = parse_fragment("<p>x</p>");
  for (const char * label : {"utf-8", "UTF-8", "utf8", " Utf-8 ", "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8",
           "x-unicode20utf8", "\tUTF8\n"}) {
    SCOPED_TRACE(label);
    Opts o = defaults();
    o.encoding = label;
    EXPECT_EQ(children(p.top(), o), "<p>x</p>");
  }
}

// ---- meta charset ----------------------------------------------------------------------------------

TEST(HtmlSerializeOptions, InjectMetaCharsetThroughTheTree) {
  Opts o = defaults();
  o.inject_meta_charset = true;
  o.encoding = "utf-8";
  struct {
    const char * html;
    const char * out;
  } cases[] = {
      // no meta: one is the first thing in the head
      {"<head></head>", "<head><meta charset=\"utf-8\"></head>"},
      {"<head><title>foo</title></head>", "<head><meta charset=\"utf-8\"><title>foo</title></head>"},
      // a meta with a charset: its value is replaced and nothing is added
      {"<head><meta charset=ascii></head>", "<head><meta charset=\"utf-8\"></head>"},
      {"<head><meta charset=ascii><meta charset=ascii></head>", "<head><meta charset=\"utf-8\"><meta charset=\"utf-8\"></head>"},
      {"<head><meta name=robots content=noindex></head>",
          "<head><meta charset=\"utf-8\"><meta name=\"robots\" content=\"noindex\"></head>"},
      {"<head><meta name=robots content=noindex><meta charset=ascii></head>",
          "<head><meta name=\"robots\" content=\"noindex\"><meta charset=\"utf-8\"></head>"},
      {"<head><meta http-equiv=content-type content='text/html; charset=ascii'></head>",
          "<head><meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\"></head>"},
      {"<head><meta http-equiv=Content-Type content='text/html; charset=ascii'></head>",
          "<head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\"></head>"},
      {"<head><meta CHARSET=ascii></head>", "<head><meta charset=\"utf-8\"></head>"},
      // an http-equiv with no content is not an encoding declaration
      {"<head><meta http-equiv=content-type></head>",
          "<head><meta charset=\"utf-8\"><meta http-equiv=\"content-type\"></head>"},
      {"<head><meta http-equiv=refresh content=5></head>",
          "<head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"5\"></head>"},
      // text, comments, nested elements inside the head
      {"<head><!--c--><script>a<b</script><noscript><link></noscript></head>",
          "<head><meta charset=\"utf-8\"><!--c--><script>a<b</script><noscript><link></noscript></head>"},
      // not in the head: not touched
      {"<head></head><body><meta charset=ascii></body>",
          "<head><meta charset=\"utf-8\"></head><body><meta charset=\"ascii\"></body>"},
  };
  for (auto & c : cases) {
    SCOPED_TRACE(c.html);
    Parsed p = parse_document(c.html);
    Out out = serialize_with(find(p.top(), "html"), CHILDREN, o);
    ASSERT_EQ(out.status, GTEXT_HTML_OK);
    std::string want = c.out;
    if (std::string(c.html).find("<body>") == std::string::npos) {
      want += "<body></body>";
    }
    EXPECT_EQ(out.bytes, want);
  }
  // The result says what it did.
  Parsed p = parse_document("<head><meta charset=a><meta charset=b></head>");
  Out out = serialize_with(find(p.top(), "html"), CHILDREN, o);
  EXPECT_EQ(out.r.meta_charset_replaced, 2u);
  EXPECT_EQ(out.r.meta_charset_injected, 0u);
  Parsed q = parse_document("<head></head>");
  out = serialize_with(find(q.top(), "html"), CHILDREN, o);
  EXPECT_EQ(out.r.meta_charset_replaced, 0u);
  EXPECT_EQ(out.r.meta_charset_injected, 1u);
  // Without an encoding there is nothing to say, and nothing is held back.
  Opts none = defaults();
  none.inject_meta_charset = true;
  EXPECT_EQ(children(find(q.top(), "html"), none), "<head></head><body></body>");
  // With an encoding and no switch, nothing is changed.
  Opts off = defaults();
  off.encoding = "utf-8";
  EXPECT_EQ(children(find(q.top(), "html"), off), "<head></head><body></body>");
  // The children of a head are what its contents are: the declaration goes first in them. An empty head too.
  EXPECT_EQ(children(find(q.top(), "head"), o), "<meta charset=\"utf-8\">");
  Parsed r = parse_document("<head><meta charset=a><title>t</title></head>");
  EXPECT_EQ(children(find(r.top(), "head"), o), "<meta charset=\"utf-8\"><title>t</title>");
  // A document with no head element is not given one.
  Build b;
  Node * html = b.el(nullptr, "html");
  b.el(html, "body");
  EXPECT_EQ(children(html, o), "<body></body>");
  // The declaration is written through the options of the attributes.
  Opts quoting = o;
  quoting.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
  quoting.use_trailing_solidus = true;
  quoting.space_before_trailing_solidus = true;
  EXPECT_EQ(children(find(q.top(), "head"), quoting), "<meta charset=utf-8 />");
}

TEST(HtmlSerializeOptions, TheEncodingIsDeclaredByItsNameInLowerCase) {
  Opts o = defaults();
  o.inject_meta_charset = true;
  o.encoding = "  UTF8 ";
  Parsed p = parse_document("<head><meta http-equiv=content-type content=x></head>");
  EXPECT_EQ(children(find(p.top(), "head"), o), "<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\">");
  Parsed q = parse_document("<head></head>");
  EXPECT_EQ(children(find(q.top(), "head"), o), "<meta charset=\"utf-8\">");
  // a label is not the name: the name of the encoding it names is what is written
  o.encoding = "SJIS";
  EXPECT_EQ(children(find(q.top(), "head"), o), "<meta charset=\"shift_jis\">");
}

// ---- optional tags -----------------------------------------------------------------------------------

Opts omit() {
  Opts o = defaults();
  o.omit_optional_tags = true;
  return o;
}

TEST(HtmlSerializeOptional, TheStandardsOwnExamples) {
  // 13.1.2.4: "<!DOCTYPE HTML><title>Hello</title><p>Welcome to this example."
  const std::string full =
      "<!DOCTYPE HTML><html><head><title>Hello</title></head><body><p>Welcome to this example.</p></body></html>";
  Parsed p = parse_document(full);
  Out out = serialize_with(p.top(), CHILDREN, omit());
  EXPECT_EQ(out.bytes, "<!DOCTYPE html><title>Hello</title><p>Welcome to this example.");
  // The standard: "<!DOCTYPE HTML><title>Hello</title><p>Welcome to this example." (the parser reads the name in
  // lower case); the p's end tag goes because there is no more content in its parent.
  Parsed q = parse_document("<!DOCTYPE HTML><html lang=en><head><title>Hello</title></head><body class=demo><p>Welcome to this example.");
  out = serialize_with(q.top(), CHILDREN, omit());
  EXPECT_EQ(out.bytes, "<!DOCTYPE html><html lang=\"en\"><title>Hello</title><body class=\"demo\"><p>Welcome to this example.");
  // What was left out is counted.
  Out first = serialize_with(p.top(), CHILDREN, omit());
  EXPECT_EQ(first.r.omitted_start_tags, 3u);  // html, head, body
  EXPECT_EQ(first.r.omitted_end_tags, 4u);    // head, p, body, html
}

struct OptionalCase {
  const char * html;   // a fragment, in the context div unless it says otherwise
  const char * out;
};

TEST(HtmlSerializeOptional, EndTagsOfLists) {
  const OptionalCase cases[] = {
      {"<ul><li>a<li>b</ul>", "<ul><li>a<li>b</ul>"},
      {"<ul><li>a</li></ul>x", "<ul><li>a</ul>x"},
      {"<ul><li>a<ul><li>b</ul><li>c</ul>", "<ul><li>a<ul><li>b</ul><li>c</ul>"},
      {"<dl><dt>t<dd>d<dt>t2<dd>d2</dl>", "<dl><dt>t<dd>d<dt>t2<dd>d2</dl>"},
      {"<dl><dt>t</dt></dl>", "<dl><dt>t</dt></dl>"},          // a dt's end tag stays at the end of its parent
      {"<dl><dd>d</dd></dl>", "<dl><dd>d</dl>"},
      {"<ruby>a<rp>(<rt>b<rp>)</ruby>", "<ruby>a<rp>(<rt>b<rp>)</ruby>"},
      {"<select><option>a<option>b<optgroup label=g><option>c</select>",
          "<select><option>a<option>b<optgroup label=\"g\"><option>c</select>"},
      {"<select><optgroup label=a><option>x</optgroup><optgroup label=b></select>",
          "<select><optgroup label=\"a\"><option>x<optgroup label=\"b\"></select>"},
      {"<select><option>a</option><hr><option>b</select>", "<select><option>a<hr><option>b</select>"},
      {"<select><optgroup label=a></optgroup><hr></select>", "<select><optgroup label=\"a\"><hr></select>"},
  };
  for (const OptionalCase & c : cases) {
    SCOPED_TRACE(c.html);
    Parsed p = parse_fragment(c.html);
    EXPECT_EQ(children(p.top(), omit()), c.out);
  }
}

TEST(HtmlSerializeOptional, EndTagsOfP) {
  const OptionalCase cases[] = {
      {"<p>a<p>b", "<p>a<p>b"},
      {"<p>a</p><div>b</div>", "<p>a<div>b</div>"},
      {"<p>a</p><ul><li>b</ul>", "<p>a<ul><li>b</ul>"},
      {"<p>a</p><span>b</span>", "<p>a</p><span>b</span>"},         // a span does not end a p
      {"<p>a</p><table></table>", "<p>a<table></table>"},
      {"<p>a</p><hr>", "<p>a<hr>"},
      {"<p>a</p><search></search>", "<p>a<search></search>"},
      {"<p>a</p><dialog></dialog>", "<p>a<dialog></dialog>"},
      {"<p>a</p><dir></dir>", "<p>a</p><dir></dir>"},               // not in the standard's list (html5lib has dir)
      {"<p>a</p>b", "<p>a</p>b"},
      {"<div><p>a</div>", "<div><p>a</div>"},
      {"<p>a</p><!--c-->", "<p>a</p><!--c-->"},
  };
  for (const OptionalCase & c : cases) {
    SCOPED_TRACE(c.html);
    Parsed p = parse_fragment(c.html);
    EXPECT_EQ(children(p.top(), omit()), c.out);
  }
}

TEST(HtmlSerializeOptional, APsEndTagStaysAtTheEndOfAParentThatForbidsItsOmission) {
  // "... and the parent element is an HTML element that is not an a, audio, del, ins, map, noscript, or video
  // element, or an autonomous custom element."
  for (const char * parent : {"a", "audio", "del", "ins", "map", "video", "noscript", "my-element", "x-y-z"}) {
    SCOPED_TRACE(parent);
    Build b;
    Node * pe = b.el(nullptr, parent);
    Node * p = b.el(pe, "p");
    b.text(p, "t");
    Opts o = omit();
    if (std::string(parent) == "noscript") {
      o.scripting = false;
    }
    EXPECT_EQ(outer(pe, o), std::string("<") + parent + "><p>t</p></" + parent + ">");
  }
  for (const char * parent : {"div", "span", "b", "My-Element", "x_y", "annotation-xml", "font-face"}) {
    SCOPED_TRACE(parent);
    Build b;
    Node * pe = b.el(nullptr, parent);
    Node * p = b.el(pe, "p");
    b.text(p, "t");
    EXPECT_EQ(outer(pe, omit()), std::string("<") + parent + "><p>t</" + parent + ">");
  }
  // The same through the stream's context: the children of an a are written with their parent unseen.
  Build b;
  Node * top = b.fragment();
  Node * a = b.el(top, "a");
  Node * p = b.el(a, "p");
  b.text(p, "t");
  EXPECT_EQ(children(a, omit()), "<p>t</p>");
  Node * div = b.el(top, "div");
  Node * q = b.el(div, "p");
  b.text(q, "t");
  EXPECT_EQ(children(div, omit()), "<p>t");
}

TEST(HtmlSerializeOptional, TableTags) {
  const OptionalCase cases[] = {
      {"<table><caption>c</caption><colgroup><col></colgroup><thead><tr><th>h</thead>"
       "<tbody><tr><td>x<td>y<tr><td>z</tbody><tfoot><tr><td>f</tfoot></table>",
          "<table><caption>c<col><thead><tr><th>h<tbody><tr><td>x<td>y<tr><td>z<tfoot><tr><td>f</table>"},
      // the tbody's start tag stays after a thead whose end tag went, and after a tbody whose end tag went
      {"<table><thead><tr><td>a</thead><tbody><tr><td>b</tbody><tbody><tr><td>c</tbody></table>",
          "<table><thead><tr><td>a<tbody><tr><td>b<tbody><tr><td>c</table>"},
      {"<table><tbody><tr><td>a</table>", "<table><tr><td>a</table>"},
      {"<table><tbody></tbody></table>", "<table><tbody></table>"},               // empty: the start tag cannot go
      {"<table><colgroup></colgroup></table>", "<table><colgroup></table>"},      // empty: the start tag cannot go
      {"<table><colgroup><col></colgroup><colgroup><col></colgroup></table>",
          "<table><col><colgroup><col></table>"},                                 // an omitted colgroup end: the next stays
      {"<table><colgroup span=2></colgroup></table>", "<table><colgroup span=\"2\"></table>"},
      {"<table><caption>a</caption> <thead></thead></table>", "<table><caption>a</caption> <thead></thead></table>"},
      {"<table><tr><td>a</td><th>b</th></tr></table>", "<table><tr><td>a<th>b</table>"},
  };
  for (const OptionalCase & c : cases) {
    SCOPED_TRACE(c.html);
    Parsed p = parse_fragment(c.html);
    EXPECT_EQ(children(p.top(), omit()), c.out);
  }
}

TEST(HtmlSerializeOptional, HeadAndBodyAndHtml) {
  struct {
    const char * html;
    const char * out;
  } cases[] = {
      {"<html><head></head><body></body></html>", ""},
      {"<html><head><title>t</title></head><body>b</body></html>", "<title>t</title>b"},
      {"<html><head> </head><body> b</body></html>", "<head> <body> b"},
      {"<html><head><!--c--></head><body><!--d--></body></html>", "<head><!--c--><body><!--d-->"},
      {"<html><head></head><body><meta></body></html>", "<body><meta>"},
      {"<html><head></head><body><script></script></body></html>", "<body><script></script>"},
      {"<html><head></head><body><style></style></body></html>", "<body><style></style>"},
      {"<html><head></head><body><noscript></noscript></body></html>", "<body><noscript></noscript>"},
      {"<html><head></head><body><link></body></html>", "<body><link>"},
      {"<html><head></head><body><template></template></body></html>", "<body><template></template>"},
      {"<html lang=en><head></head><body></body></html>", "<html lang=\"en\">"},
      {"<html><head class=x></head><body></body></html>", "<head class=\"x\">"},
      {"<html><head></head><body id=y></body></html>", "<body id=\"y\">"},
      {"<html><head></head><body>b</body></html><!--after-->", "b</html><!--after-->"},
      {"<!--before--><html><head></head><body>b</body></html>", "<!--before-->b"},
      {"<html><!--first--><head></head><body>b</body></html>", "<html><!--first-->b"},
  };
  for (auto & c : cases) {
    SCOPED_TRACE(c.html);
    Parsed p = parse_document(c.html);
    Out out = serialize_with(p.top(), CHILDREN, omit());
    // The comment before the html element is a child of the document, and after it too.
    EXPECT_EQ(out.bytes, c.out);
  }
}

TEST(HtmlSerializeOptional, AnOmittedTagIsReadBackAsTheSameTreeForConformingInput) {
  const char * const docs[] = {
      "<!DOCTYPE html><html><head><title>t</title></head><body><p>a<p>b<ul><li>c<li>d</ul><table><tr><td>e<td>f</table></body></html>",
      "<!DOCTYPE html><html lang=en><head><meta charset=utf-8><title>t</title></head><body class=x><dl><dt>a<dd>b</dl></body></html>",
      "<!DOCTYPE html><html><head></head><body><select><option>a<option>b</select><ruby>x<rp>(<rt>y<rp>)</ruby></body></html>",
      "<!DOCTYPE html><html><head><title>t</title></head><body><table><caption>c<colgroup><col><thead><tr><th>h<tbody><tr><td>d<tfoot><tr><td>f</table></body></html>",
  };
  for (const char * doc : docs) {
    SCOPED_TRACE(doc);
    Parsed a = parse_document(doc);
    Out out = serialize_with(a.top(), CHILDREN, omit());
    ASSERT_EQ(out.status, GTEXT_HTML_OK);
    EXPECT_GT(out.r.omitted_start_tags + out.r.omitted_end_tags, 0u);
    Parsed b = parse_document(out.bytes);
    EXPECT_TRUE(gtext_markup_equal(a.top(), b.top())) << out.bytes;
    // And idempotent.
    EXPECT_EQ(serialize_with(b.top(), CHILDREN, omit()).bytes, out.bytes);
  }
}

// ---- the token stream --------------------------------------------------------------------------------------

struct Stream {
  Collect sink;
  GTEXT_HTML_Serializer * ser = nullptr;
  explicit Stream(const Opts & o = defaults()) {
    GTEXT_Markup_Sink s = sink.sink();
    EXPECT_EQ(gtext_html_serializer_new(&o, &s, &ser), GTEXT_HTML_OK);
  }
  ~Stream() {
    gtext_html_serializer_free(ser);
  }
  void start(const char * name, std::vector<GTEXT_Markup_Attribute> attrs = {}, const char * ns = nullptr) {
    EXPECT_EQ(gtext_html_serializer_start_tag(ser, ns, name, attrs.data(), attrs.size()), GTEXT_HTML_OK);
  }
  void empty(const char * name, std::vector<GTEXT_Markup_Attribute> attrs = {}) {
    EXPECT_EQ(gtext_html_serializer_empty_tag(ser, nullptr, name, attrs.data(), attrs.size()), GTEXT_HTML_OK);
  }
  void end(const char * name, const char * ns = nullptr) {
    EXPECT_EQ(gtext_html_serializer_end_tag(ser, ns, name), GTEXT_HTML_OK);
  }
  void chars(const std::string & s) {
    EXPECT_EQ(gtext_html_serializer_characters(ser, s.data(), s.size()), GTEXT_HTML_OK);
  }
  void comment(const std::string & s) {
    EXPECT_EQ(gtext_html_serializer_comment(ser, s.data(), s.size()), GTEXT_HTML_OK);
  }
  std::string finish(GTEXT_HTML_Serialize_Result * r = nullptr) {
    EXPECT_EQ(gtext_html_serializer_finish(ser, r), GTEXT_HTML_OK);
    return sink.data;
  }
};

GTEXT_Markup_Attribute attribute(const char * local, const char * value) {
  GTEXT_Markup_Attribute a;
  std::memset(&a, 0, sizeof(a));
  a.local = local;
  a.value = value;
  a.value_length = std::strlen(value);
  return a;
}

TEST(HtmlSerializeStream, TokensAreWrittenAsTheTreeWalkWritesThem) {
  Stream s;
  s.start("p", {attribute("class", "a\"b")});
  s.chars("x<y");
  s.empty("br");
  s.start("img", {attribute("src", "i")});   // a void element given as a start tag
  s.end("img");                               // ... and its end tag is dropped
  s.comment("c");
  s.end("p");
  EXPECT_EQ(s.finish(), "<p class=\"a&quot;b\">x&lt;y<br><img src=\"i\"><!--c--></p>");
}

TEST(HtmlSerializeStream, UnbalancedAndEmptyStreamsAreWrittenAsTheyAre) {
  {
    Stream s;
    EXPECT_EQ(s.finish(), "");
  }
  {
    Stream s;
    s.end("div");
    s.end("div");
    s.start("a");
    EXPECT_EQ(s.finish(), "</div></div><a>");
  }
  {
    Stream s;
    s.chars("");
    s.chars("a");
    s.chars("");
    EXPECT_EQ(s.finish(), "a");
  }
}

TEST(HtmlSerializeStream, TheContextDecidesRawTextAndThePOmission) {
  {
    Stream s;
    s.chars("a<b");
    EXPECT_EQ(s.finish(), "a&lt;b");
  }
  {
    Stream s;
    EXPECT_EQ(gtext_html_serializer_context(s.ser, nullptr, "script"), GTEXT_HTML_OK);
    s.chars("a<b");
    EXPECT_EQ(s.finish(), "a<b");
  }
  {
    Stream s(omit());
    EXPECT_EQ(gtext_html_serializer_context(s.ser, nullptr, "a"), GTEXT_HTML_OK);
    s.start("p");
    s.chars("x");
    s.end("p");
    EXPECT_EQ(s.finish(), "<p>x</p>");
  }
  {
    Stream s(omit());
    EXPECT_EQ(gtext_html_serializer_context(s.ser, nullptr, "div"), GTEXT_HTML_OK);
    s.start("p");
    s.chars("x");
    s.end("p");
    EXPECT_EQ(s.finish(), "<p>x");
  }
  {
    // The context is not a token: it is refused after one, and after the end.
    Stream s;
    s.chars("x");
    EXPECT_EQ(gtext_html_serializer_context(s.ser, nullptr, "div"), GTEXT_HTML_E_STATE);
    s.finish();
    EXPECT_EQ(gtext_html_serializer_context(s.ser, nullptr, "div"), GTEXT_HTML_E_STATE);
  }
  // A context in another namespace is not an HTML script.
  {
    Stream s;
    EXPECT_EQ(gtext_html_serializer_context(s.ser, GTEXT_MARKUP_NS_SVG, "script"), GTEXT_HTML_OK);
    s.chars("a<b");
    EXPECT_EQ(s.finish(), "a&lt;b");
  }
}

TEST(HtmlSerializeStream, ArgumentsAreChecked) {
  Stream s;
  EXPECT_EQ(gtext_html_serializer_start_tag(s.ser, nullptr, nullptr, nullptr, 0), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_start_tag(s.ser, nullptr, "", nullptr, 0), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_start_tag(s.ser, nullptr, "a", nullptr, 1), GTEXT_HTML_E_INVALID);
  GTEXT_Markup_Attribute bad = attribute("x", "v");
  bad.local = nullptr;
  EXPECT_EQ(gtext_html_serializer_start_tag(s.ser, nullptr, "a", &bad, 1), GTEXT_HTML_E_INVALID);
  bad = attribute("", "v");
  EXPECT_EQ(gtext_html_serializer_empty_tag(s.ser, nullptr, "a", &bad, 1), GTEXT_HTML_E_INVALID);
  bad = attribute("x", "v");
  bad.value = nullptr;
  bad.value_length = 3;
  EXPECT_EQ(gtext_html_serializer_start_tag(s.ser, nullptr, "a", &bad, 1), GTEXT_HTML_E_INVALID);
  bad.value_length = 0;   // a NULL value of length 0 is an empty value
  EXPECT_EQ(gtext_html_serializer_start_tag(s.ser, nullptr, "a", &bad, 1), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_serializer_end_tag(s.ser, nullptr, nullptr), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_end_tag(s.ser, nullptr, ""), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_characters(s.ser, nullptr, 1), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_characters(s.ser, nullptr, 0), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_serializer_comment(s.ser, nullptr, 1), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_comment(s.ser, nullptr, 0), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_serializer_processing_instruction(s.ser, nullptr, "d", 1), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_processing_instruction(s.ser, "t", nullptr, 1), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_processing_instruction(s.ser, "t", nullptr, 0), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_serializer_doctype(s.ser, nullptr, nullptr, nullptr), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_serializer_start_tag(nullptr, nullptr, "a", nullptr, 0), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_end_tag(nullptr, nullptr, "a"), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_characters(nullptr, "a", 1), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_finish(nullptr, nullptr), GTEXT_HTML_E_INVALID);
  gtext_html_serializer_free(nullptr);
  // None of the refusals was a token.
  EXPECT_EQ(s.finish(), "<a x=\"\"><!----><?t ?><!DOCTYPE >");
  // new
  GTEXT_Markup_Sink sink = {nullptr, nullptr};
  GTEXT_HTML_Serializer * out = reinterpret_cast<GTEXT_HTML_Serializer *>(0x1);
  EXPECT_EQ(gtext_html_serializer_new(nullptr, &sink, &out), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(gtext_html_serializer_new(nullptr, nullptr, &out), GTEXT_HTML_E_INVALID);
  GTEXT_Markup_Sink ok = s.sink.sink();
  EXPECT_EQ(gtext_html_serializer_new(nullptr, &ok, nullptr), GTEXT_HTML_E_INVALID);
}

TEST(HtmlSerializeStream, AfterFinishEveryTokenIsAStateError) {
  Stream s;
  s.start("a");
  EXPECT_EQ(s.finish(), "<a>");
  EXPECT_EQ(gtext_html_serializer_start_tag(s.ser, nullptr, "b", nullptr, 0), GTEXT_HTML_E_STATE);
  EXPECT_EQ(gtext_html_serializer_end_tag(s.ser, nullptr, "a"), GTEXT_HTML_E_STATE);
  EXPECT_EQ(gtext_html_serializer_characters(s.ser, "x", 1), GTEXT_HTML_E_STATE);
  EXPECT_EQ(gtext_html_serializer_comment(s.ser, "x", 1), GTEXT_HTML_E_STATE);
  EXPECT_EQ(gtext_html_serializer_doctype(s.ser, "x", nullptr, nullptr), GTEXT_HTML_E_STATE);
  EXPECT_EQ(gtext_html_serializer_finish(s.ser, nullptr), GTEXT_HTML_E_STATE);
  EXPECT_EQ(s.sink.data, "<a>");
}

TEST(HtmlSerializeStream, ANonHtmlNamespaceHasNoVoidRawTextOrOptionalTags) {
  Stream s(omit());
  s.start("p", {}, GTEXT_MARKUP_NS_SVG);
  s.start("br", {}, GTEXT_MARKUP_NS_SVG);
  s.end("br", GTEXT_MARKUP_NS_SVG);
  s.end("p", GTEXT_MARKUP_NS_SVG);
  s.start("html", {}, "urn:x");
  s.end("html", "urn:x");
  EXPECT_EQ(s.finish(), "<p><br></br></p><html></html>");
}

TEST(HtmlSerializeStream, TheResultIsClearedAndCountsWhatTheStreamDid) {
  Opts o = omit();
  o.minimize_boolean_attributes = true;
  Stream s(o);
  s.start("html");
  s.start("head");
  s.end("head");
  s.start("body");
  s.start("input", {attribute("disabled", "disabled")});
  s.end("body");
  s.end("html");
  GTEXT_HTML_Serialize_Result r;
  std::memset(&r, 0x55, sizeof(r));
  EXPECT_EQ(s.finish(&r), "<input disabled>");
  EXPECT_EQ(r.status, GTEXT_HTML_OK);
  EXPECT_EQ(r.bytes_written, 16u);
  EXPECT_EQ(r.omitted_start_tags, 3u);
  EXPECT_EQ(r.omitted_end_tags, 3u);
  EXPECT_EQ(r.minimized_attributes, 1u);
  EXPECT_EQ(r.nodes_visited, 0u);
  EXPECT_NE(r.message, nullptr);
}


// ---- review findings -----------------------------------------------------------------------------------------------

TEST(HtmlSerializeReview, AProcessingInstructionIsWrittenWithItsClosingQuestionMarkAndRoundTrips) {
  Stream s;
  EXPECT_EQ(gtext_html_serializer_processing_instruction(s.ser, "xml-stylesheet", "href=a", 6), GTEXT_HTML_OK);
  EXPECT_EQ(s.finish(), "<?xml-stylesheet href=a?>");
  Parsed p = parse_fragment("<?t d?>x");
  Node * pi = gtext_markup_node_first_child(p.top());
  ASSERT_NE(pi, nullptr);
  if (gtext_markup_node_kind(pi) == GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION) {
    EXPECT_EQ(children(p.top()), "<?t d?>x");
  }
  const std::string once = children(p.top());
  Parsed again = parse_fragment(once);
  EXPECT_EQ(children(again.top()), once);
}

TEST(HtmlSerializeReview, AStrayEndTagIsWrittenAndPopsNothingItDoesNotMatch) {
  Opts o = defaults();
  o.strip_whitespace = true;
  {
    Stream s(o);
    s.end("pre"); // nothing is open: written, and it must not open a window of preserved whitespace
    s.chars("a   b");
    EXPECT_EQ(s.finish(), "</pre>a b");
  }
  {
    Stream s(o);
    s.start("pre");
    s.start("b");
    s.end("pre"); // does not match the top (b): written, pops nothing
    s.chars("a   b");
    s.end("b");
    s.end("pre");
    s.chars("c   d");
    EXPECT_EQ(s.finish(), "<pre><b></pre>a   b</b></pre>c d");
  }
  {
    Stream s(o);
    s.start("pre");
    s.end("pre");
    s.end("pre"); // a second one, with nothing open
    s.chars("a   b");
    EXPECT_EQ(s.finish(), "<pre></pre></pre>a b");
  }
}

TEST(HtmlSerializeReview, TheHeldHeadIsBoundedByTheOutputSizeForTagPairs) {
  for (const char * kind : {"pairs", "void"}) {
    for (size_t n : {1u, 2u, 10u, 300u}) {
      Opts o = defaults();
      o.inject_meta_charset = true;
      o.encoding = "utf-8";
      auto run = [&](size_t limit, std::string * got) {
        Opts q = o;
        q.max_output_bytes = limit;
        Collect c;
        GTEXT_Markup_Sink sk = c.sink();
        GTEXT_HTML_Serializer * ser = nullptr;
        EXPECT_EQ(gtext_html_serializer_new(&q, &sk, &ser), GTEXT_HTML_OK);
        GTEXT_HTML_Status st = gtext_html_serializer_start_tag(ser, nullptr, "head", nullptr, 0);
        for (size_t i = 0; i < n && st == GTEXT_HTML_OK; i++) {
          if (std::strcmp(kind, "pairs") == 0) {
            st = gtext_html_serializer_start_tag(ser, nullptr, "b", nullptr, 0);
            if (st == GTEXT_HTML_OK) {
              st = gtext_html_serializer_end_tag(ser, nullptr, "b");
            }
          }
          else {
            st = gtext_html_serializer_empty_tag(ser, nullptr, "br", nullptr, 0);
          }
        }
        if (st == GTEXT_HTML_OK) {
          st = gtext_html_serializer_end_tag(ser, nullptr, "head");
        }
        if (st == GTEXT_HTML_OK) {
          st = gtext_html_serializer_finish(ser, nullptr);
        }
        gtext_html_serializer_free(ser);
        *got = c.data;
        return st;
      };
      std::string full;
      ASSERT_EQ(run(0, &full), GTEXT_HTML_OK) << kind << n;
      std::string got;
      EXPECT_EQ(run(full.size(), &got), GTEXT_HTML_OK) << kind << n;
      EXPECT_EQ(got, full);
      EXPECT_EQ(run(full.size() - 1, &got), GTEXT_HTML_E_LIMIT_OUTPUT) << kind << n;
      EXPECT_LE(got.size(), full.size() - 1);
    }
  }
}

TEST(HtmlSerializeReview, AFailureDeliversTheSamePrefixFromTheWalkAndTheTokenStream) {
  Parsed p = parse_fragment("<p class=a>one</p><p>two<b>three</b></p>");
  Opts o = defaults();
  Collect whole;
  GTEXT_Markup_Sink ws = whole.sink();
  ASSERT_EQ(gtext_html_serialize(p.top(), CHILDREN, &o, &ws, nullptr), GTEXT_HTML_OK);
  for (size_t limit = 1; limit < whole.data.size(); limit++) {
    o.max_output_bytes = limit;
    Collect a;
    GTEXT_Markup_Sink as = a.sink();
    EXPECT_EQ(gtext_html_serialize(p.top(), CHILDREN, &o, &as, nullptr), GTEXT_HTML_E_LIMIT_OUTPUT);
    Collect b;
    GTEXT_Markup_Sink bs = b.sink();
    GTEXT_HTML_Serializer * ser = nullptr;
    ASSERT_EQ(gtext_html_serializer_new(&o, &bs, &ser), GTEXT_HTML_OK);
    GTEXT_HTML_Status st = gtext_html_serializer_context(ser, nullptr, "div");
    GTEXT_Markup_Attribute cls = attribute("class", "a");
    auto tag = [&](const char * n, const GTEXT_Markup_Attribute * at, size_t c) {
      if (st == GTEXT_HTML_OK) st = gtext_html_serializer_start_tag(ser, nullptr, n, at, c);
    };
    auto etag = [&](const char * n) {
      if (st == GTEXT_HTML_OK) st = gtext_html_serializer_end_tag(ser, nullptr, n);
    };
    auto txt = [&](const char * t) {
      if (st == GTEXT_HTML_OK) st = gtext_html_serializer_characters(ser, t, std::strlen(t));
    };
    tag("p", &cls, 1); txt("one"); etag("p");
    tag("p", nullptr, 0); txt("two"); tag("b", nullptr, 0); txt("three"); etag("b"); etag("p");
    if (st == GTEXT_HTML_OK) st = gtext_html_serializer_finish(ser, nullptr);
    EXPECT_EQ(st, GTEXT_HTML_E_LIMIT_OUTPUT) << limit;
    gtext_html_serializer_free(ser);
    EXPECT_EQ(a.data, b.data) << limit;
    EXPECT_LE(a.data.size(), limit);
  }
}

TEST(HtmlSerializeReview, ADoctypeIdentifierWithBothQuotesCannotBeWritten) {
  Opts o = defaults();
  o.doctype_identifiers = true;
  Stream s(o);
  EXPECT_EQ(gtext_html_serializer_doctype(s.ser, "html", "a\"b'c", nullptr), GTEXT_HTML_E_INVALID);
  Stream t(o);
  EXPECT_EQ(gtext_html_serializer_doctype(t.ser, "html", "a\"b", "c'd"), GTEXT_HTML_OK);
  EXPECT_EQ(t.finish(), "<!DOCTYPE html PUBLIC 'a\"b' \"c'd\">");
}

TEST(HtmlSerializeReview, TheMetaPlanIsCaseInsensitiveAndAHeadIsOnlyEnteredBeforeAnyOther) {
  Opts o = defaults();
  o.inject_meta_charset = true;
  o.encoding = "utf-8";
  {
    GTEXT_Markup_Attribute a[1] = {attribute("CHARSET", "x")};
    Stream s(o);
    s.start("head");
    EXPECT_EQ(gtext_html_serializer_empty_tag(s.ser, nullptr, "meta", a, 1), GTEXT_HTML_OK);
    s.end("head");
    GTEXT_HTML_Serialize_Result r;
    s.finish(&r);
    EXPECT_EQ(r.meta_charset_replaced + r.meta_charset_injected, 1u);
  }
  {
    // A second head is not a second place to inject.
    Stream s(o);
    s.start("head");
    s.end("head");
    s.start("head");
    s.end("head");
    GTEXT_HTML_Serialize_Result r;
    s.finish(&r);
    EXPECT_EQ(r.meta_charset_injected, 1u);
    EXPECT_EQ(r.meta_charset_replaced, 0u);
  }
}

TEST(HtmlSerializeReview, AStreamIsNotStartedByAnArgumentThatWasRefused) {
  Stream s;
  EXPECT_EQ(gtext_html_serializer_start_tag(s.ser, nullptr, nullptr, nullptr, 0), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_serializer_context(s.ser, nullptr, "script"), GTEXT_HTML_OK);
}

TEST(HtmlSerializeReview, TheContextOptionSetsTheParentAtCreation) {
  Opts o = defaults();
  o.context_name = "script";
  Stream s(o);
  s.chars("a<b");
  EXPECT_EQ(s.finish(), "a<b");
  Opts bad = defaults();
  bad.context_namespace = "http://www.w3.org/2000/svg";
  Collect c;
  GTEXT_Markup_Sink sk = c.sink();
  GTEXT_HTML_Serializer * ser = nullptr;
  EXPECT_EQ(gtext_html_serializer_new(&bad, &sk, &ser), GTEXT_HTML_E_OPTIONS);
}

TEST(HtmlSerializeReview, AFragmentIsWrittenAsTheInnerHtmlOfItsContextWithoutCopyingTheTree) {
  Parsed p = parse_fragment("a<b&c", "script");
  EXPECT_EQ(children(p.top()), "a&lt;b&amp;c"); // no context: escaped
  Opts o = defaults();
  o.context_name = "script";
  EXPECT_EQ(children(p.top(), o), "a<b&c");
  EXPECT_EQ(outer(p.top(), o), "a<b&c");
  Parsed q = parse_fragment("a&lt;b&amp;c", "div");
  o.context_name = "div";
  EXPECT_EQ(children(q.top(), o), "a&lt;b&amp;c");
}

TEST(HtmlSerializeReview, TemplateContentIsAScopeWhoseParentIsTheTemplate) {
  Parsed p = parse_fragment("<template><script>a<b</script><p>x</p></template>");
  Node * tpl = find(p.top(), "template");
  Node * content = gtext_markup_element_template_content(tpl);
  ASSERT_NE(content, nullptr);
  EXPECT_EQ(children(content), "<script>a<b</script><p>x</p>");
  EXPECT_EQ(children(tpl), "<script>a<b</script><p>x</p>");
  Node * scr = find(content, "script");
  EXPECT_EQ(outer(gtext_markup_node_first_child(scr)), "a<b");
}

} // namespace
