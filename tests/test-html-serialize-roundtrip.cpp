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
 * The round trip of the HTML serializer, as a property.
 *
 * For a conforming document the standard promises that parse-serialize-parse gives the tree back (13.3 says what does
 * not), and this file holds the serializer to it on documents a generator writes: nested phrasing and flow content,
 * lists, tables, forms, attribute values with every character that needs an escape, entities. Each is parsed, written
 * under each of four option sets that must not change what a parser reads (the defaults, the optional tags left out,
 * quotes only where needed with the best one, a trailing solidus), parsed again, and the trees must be equal and the
 * second write the same bytes as the first.
 *
 * Then the other side: the known cases that do not round-trip, each written down with the tree it gives back, so that
 * a change that fixed one of them (or broke the others) is seen. The tree-construction vectors are round-tripped by
 * tools/conformance/html_serializer_suite.py, which classifies the ones that do not.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>

#include "html_serialize_test_support.h"

namespace {

using namespace hs;

constexpr GTEXT_HTML_Serialize_Scope CHILDREN = GTEXT_HTML_SERIALIZE_CHILDREN;

// ---- a generator of conforming documents ---------------------------------------------------------------

struct Gen {
  uint64_t state;
  explicit Gen(uint64_t seed) : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
  uint32_t next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint32_t>(state >> 33);
  }
  uint32_t below(uint32_t n) {
    return next() % n;
  }
  bool chance(uint32_t percent) {
    return below(100) < percent;
  }

  std::string word() {
    static const char * const words[] = {"alpha", "beta", "x", "y=z", "a&amp;b", "1 &lt; 2", "&gt;", "&nbsp;", "caf\xC3\xA9",
        "\xE2\x82\xAC", "it's", "say &quot;hi&quot;", "p.q", "end.", "-->", "<-", "&copy;", "a  b", "\xF0\x9F\x98\x80"};
    return words[below(sizeof(words) / sizeof(words[0]))];
  }
  std::string text() {
    std::string s = word();
    while (chance(40)) {
      s += chance(50) ? " " : "";
      s += word();
    }
    // no leading white space after a start tag that would make a difference to the optional tags, and no
    // line feed first in a pre
    return s;
  }
  std::string attr_value() {
    static const char * const values[] = {"plain", "two words", "a\"b", "it's", "a&amp;b", "x=y", "<>", "a>b", "",
        "tab\there", "back`tick", "q\"'q", "\xC2\xA0nbsp", "/slash/", "caf\xC3\xA9"};
    return values[below(sizeof(values) / sizeof(values[0]))];
  }
  std::string quote(const std::string & v) {
    // Written so that the parser reads exactly v: in single quotes unless v has one.
    std::string out;
    if (v.find('\'') == std::string::npos) {
      return "'" + v + "'";
    }
    for (char c : v) {
      if (c == '"') {
        out += "&quot;";
      }
      else if (c == '&') {
        out += "&amp;";
      }
      else {
        out += c;
      }
    }
    // v may already hold references (it is source text): use it as it is in double quotes when it has no double quote
    if (v.find('"') == std::string::npos) {
      return "\"" + v + "\"";
    }
    return "\"" + out + "\"";
  }
  std::string attrs() {
    std::string s;
    static const char * const names[] = {"class", "id", "title", "lang", "data-a", "aria-label", "style"};
    uint32_t n = below(4);
    std::vector<int> used;
    for (uint32_t i = 0; i < n; i++) {
      int k = below(sizeof(names) / sizeof(names[0]));
      bool dup = false;
      for (int u : used) {
        dup = dup || u == k;
      }
      if (dup) {
        continue;
      }
      used.push_back(k);
      s += std::string(" ") + names[k] + "=" + quote(attr_value());
    }
    return s;
  }

  std::string phrasing(int depth, bool in_a) {
    std::string s;
    uint32_t n = 1 + below(4);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t k = below(depth > 3 ? 3 : 12);
      switch (k) {
        case 0:
        case 1:
        case 2:
          s += text();
          break;
        case 3:
          s += "<b" + attrs() + ">" + phrasing(depth + 1, in_a) + "</b>";
          break;
        case 4:
          s += "<i>" + phrasing(depth + 1, in_a) + "</i>";
          break;
        case 5:
          s += "<span" + attrs() + ">" + phrasing(depth + 1, in_a) + "</span>";
          break;
        case 6:
          s += in_a ? text() : "<a href=" + quote("/p?a=1&amp;b=" + word()) + ">" + phrasing(depth + 1, true) + "</a>";
          break;
        case 7:
          s += "<br>";
          break;
        case 8:
          s += "<img src=" + quote("i.png") + " alt=" + quote(attr_value()) + ">";
          break;
        case 9:
          s += "<code>" + phrasing(depth + 1, in_a) + "</code>";
          break;
        case 10:
          s += "<!--" + std::string(chance(50) ? "note" : " c ") + "-->";
          break;
        default:
          s += "<em>" + phrasing(depth + 1, in_a) + "</em>";
          break;
      }
    }
    return s;
  }

  std::string cells(const char * tag, int depth) {
    std::string s;
    uint32_t n = 1 + below(3);
    for (uint32_t i = 0; i < n; i++) {
      s += std::string("<") + tag + attrs() + ">" + (chance(50) ? phrasing(depth + 1, false) : flow(depth + 2)) + "</" + tag + ">";
    }
    return s;
  }

  std::string flow(int depth) {
    std::string s;
    uint32_t n = 1 + below(3);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t k = below(depth > 3 ? 4 : 13);
      switch (k) {
        case 0:
        case 1:
          s += "<p" + attrs() + ">" + phrasing(depth + 1, false) + "</p>";
          break;
        case 2:
          s += "<h2>" + phrasing(depth + 1, false) + "</h2>";
          break;
        case 3:
          s += "<hr>";
          break;
        case 4:
          s += "<div" + attrs() + ">" + flow(depth + 1) + "</div>";
          break;
        case 5: {
          s += chance(50) ? "<ul>" : "<ol>";
          std::string close = s.substr(s.size() - 3) == "<ul>" ? "</ul>" : "</ol>";
          uint32_t m = 1 + below(3);
          for (uint32_t j = 0; j < m; j++) {
            s += "<li" + attrs() + ">" + (chance(50) ? phrasing(depth + 1, false) : flow(depth + 1)) + "</li>";
          }
          s += close;
          break;
        }
        case 6: {
          s += "<dl>";
          uint32_t m = 1 + below(3);
          for (uint32_t j = 0; j < m; j++) {
            s += "<dt>" + phrasing(depth + 1, false) + "</dt><dd>" + (chance(50) ? phrasing(depth + 1, false) : flow(depth + 1)) + "</dd>";
          }
          s += "</dl>";
          break;
        }
        case 7: {
          s += "<table" + attrs() + ">";
          if (chance(30)) {
            s += "<caption>" + phrasing(depth + 1, false) + "</caption>";
          }
          if (chance(30)) {
            s += "<colgroup><col span=2></colgroup>";
          }
          if (chance(30)) {
            s += "<thead><tr>" + cells("th", depth) + "</tr></thead>";
          }
          s += chance(50) ? "<tbody>" : "";
          uint32_t m = 1 + below(3);
          for (uint32_t j = 0; j < m; j++) {
            s += "<tr>" + cells("td", depth) + "</tr>";
          }
          s += "</table>";
          break;
        }
        case 8:
          s += "<pre>" + std::string(chance(30) ? " " : "") + text() + "</pre>";
          break;
        case 9:
          s += "<blockquote>" + flow(depth + 1) + "</blockquote>";
          break;
        case 10:
          s += "<select><option value=a>" + word() + "</option><option>" + word() + "</option></select>";
          break;
        case 11:
          s += "<ruby>" + word() + "<rp>(</rp><rt>" + word() + "</rt><rp>)</rp></ruby>";
          break;
        default:
          s += "<form" + attrs() + "><label>" + word() + "<input type=text name=n" + attrs() + "></label><textarea>" + word() + "</textarea></form>";
          break;
      }
    }
    return s;
  }

  std::string document() {
    std::string s = "<!DOCTYPE html><html" + (chance(30) ? std::string(" lang=en") : "") + "><head>";
    if (chance(70)) {
      s += "<meta charset=utf-8>";
    }
    s += "<title>" + word() + "</title>";
    if (chance(30)) {
      s += "<style>a>b{c:d}</style>";
    }
    if (chance(30)) {
      s += "<script>if (a<b && c) {}</script>";
    }
    s += "</head><body" + attrs() + ">" + flow(0) + "</body></html>";
    return s;
  }
};

struct Variant {
  const char * name;
  Opts opts;
};

std::vector<Variant> variants() {
  std::vector<Variant> v;
  v.push_back({"defaults", defaults()});
  Opts omit = defaults();
  omit.omit_optional_tags = true;
  v.push_back({"omit", omit});
  Opts needed = defaults();
  needed.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
  needed.use_best_quote_char = true;
  v.push_back({"needed", needed});
  Opts solidus = defaults();
  solidus.use_trailing_solidus = true;
  solidus.space_before_trailing_solidus = true;
  solidus.alphabetical_attributes = true;
  v.push_back({"solidus", solidus});
  Opts all = defaults();
  all.omit_optional_tags = true;
  all.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
  all.use_best_quote_char = true;
  all.use_trailing_solidus = true;
  all.inject_meta_charset = true;
  all.encoding = "utf-8";
  v.push_back({"all", all});
  return v;
}

TEST(HtmlSerializeRoundTrip, ConformingDocumentsComeBackAsTheSameTreeAndTheSameBytes) {
  const std::vector<Variant> vs = variants();
  size_t trees = 0;
  size_t omitted = 0;
  for (uint64_t seed = 1; seed <= 250; seed++) {
    Gen g(seed);
    const std::string html = g.document();
    Parsed a = parse_document(html);
    for (const Variant & v : vs) {
      SCOPED_TRACE(std::string("seed ") + std::to_string(seed) + " " + v.name + ": " + html);
      Out first = serialize_with(a.top(), CHILDREN, v.opts);
      ASSERT_EQ(first.status, GTEXT_HTML_OK);
      omitted += first.r.omitted_start_tags + first.r.omitted_end_tags;
      Parsed b = parse_document(first.bytes);
      if (std::string(v.name) == "all") {
        // inject_meta_charset rewrites the head's meta (utf-8 already) or adds one: the tree gains a meta
        // when there was none, so compare after writing both the same way.
        Out second = serialize_with(b.top(), CHILDREN, v.opts);
        EXPECT_EQ(second.bytes, first.bytes) << first.bytes;
        continue;
      }
      EXPECT_TRUE(gtext_markup_equal(a.top(), b.top())) << first.bytes;
      Out second = serialize_with(b.top(), CHILDREN, v.opts);
      EXPECT_EQ(second.bytes, first.bytes);
      trees++;
    }
  }
  EXPECT_GE(trees, 1000u);
  EXPECT_GT(omitted, 1000u); // the optional tags were in play
}

TEST(HtmlSerializeRoundTrip, FragmentsInTheirContextComeBackToo) {
  const struct {
    const char * context;
    const char * html;
  } cases[] = {
      {"div", "<p class=a>x<b>y</b><ul><li>1<li>2</ul>"},
      {"table", "<tbody><tr><td>a<td>b<tr><td>c"},
      {"tr", "<td>a<td>b"},
      {"select", "<option>a<option>b<optgroup label=g><option>c"},
      {"ul", "<li>a<li>b<ul><li>c</ul>"},
      {"body", "<p>a<p>b"},
  };
  for (auto & c : cases) {
    SCOPED_TRACE(c.html);
    for (const Variant & v : variants()) {
      if (std::string(v.name) == "all") {
        continue;
      }
      Parsed a = parse_fragment(c.html, c.context);
      // The contents of an element of the context's name: what innerHTML writes.
      Build holder;
      Node * ctx = holder.el(nullptr, c.context);
      for (Node * n = gtext_markup_node_first_child(a.fragment); n; n = gtext_markup_node_next_sibling(n)) {
        Node * copy = nullptr;
        ASSERT_EQ(gtext_markup_clone(holder.doc, n, true, &copy), GTEXT_MARKUP_OK);
        ASSERT_EQ(gtext_markup_append_child(ctx, copy), GTEXT_MARKUP_OK);
      }
      Out first = serialize_with(ctx, CHILDREN, v.opts);
      ASSERT_EQ(first.status, GTEXT_HTML_OK);
      Parsed b = parse_fragment(first.bytes, c.context);
      EXPECT_TRUE(gtext_markup_equal(a.fragment, b.fragment)) << v.name << ": " << first.bytes;
    }
  }
}

// ---- what does not round-trip, written down --------------------------------------------------------------

TEST(HtmlSerializeRoundTrip, TheStandardsOwnExamplesOfTreesThatDoNotComeBack) {
  // 13.3: a form inside a form that the parser made by foster parenting.
  {
    Parsed a = parse_document("<form id=outer><div></form><form id=inner><input>");
    Out s = serialize_with(a.top(), CHILDREN, defaults());
    EXPECT_EQ(s.bytes,
        "<html><head></head><body><form id=\"outer\"><div><form id=\"inner\"><input></form></div></form></body></html>");
    Parsed b = parse_document(s.bytes);
    EXPECT_FALSE(gtext_markup_equal(a.top(), b.top()));
    EXPECT_EQ(serialize_with(b.top(), CHILDREN, defaults()).bytes,
        "<html><head></head><body><form id=\"outer\"><div><input></div></form></body></html>"); // the second form is ignored
  }
  // 13.3: `<a><table><a>`: the second a is foster parented to be a child of the first, and comes back as a sibling.
  {
    Parsed a = parse_document("<a><table><a>");
    Out s = serialize_with(a.top(), CHILDREN, defaults());
    EXPECT_EQ(s.bytes, "<html><head></head><body><a><a></a><table></table></a></body></html>");
    Parsed b = parse_document(s.bytes);
    EXPECT_EQ(serialize_with(b.top(), CHILDREN, defaults()).bytes,
        "<html><head></head><body><a></a><a></a><table></table></body></html>");
  }
  // 13.3: the line feed of a pre.
  {
    Parsed a = parse_document("<pre>\n\nHello.</pre>");
    Out s = serialize_with(a.top(), CHILDREN, defaults());
    EXPECT_EQ(s.bytes, "<html><head></head><body><pre>\nHello.</pre></body></html>");
    Parsed b = parse_document(s.bytes);
    EXPECT_EQ(serialize_with(b.top(), CHILDREN, defaults()).bytes, "<html><head></head><body><pre>Hello.</pre></body></html>");
  }
  // A script that holds its own end tag, a comment that holds "-->", a textarea that holds a comment, a p that holds a ul.
  {
    Build b;
    Node * f = b.fragment();
    Node * script = b.el(f, "script");
    b.text(script, "</script><b>x");
    Node * c = b.el(f, "div");
    b.comment(c, "a-->b");
    Node * ta = b.el(f, "textarea");
    b.comment(ta, "c");
    Node * p = b.el(f, "p");
    b.el(p, "ul");
    const std::string s = children(f);
    EXPECT_EQ(s, "<script></script><b>x</script><div><!--a-->b--></div><textarea><!--c--></textarea><p><ul></ul></p>");
    Parsed back = parse_fragment(s);
    EXPECT_FALSE(gtext_markup_equal(f, back.fragment));
    // ... and each of them comes back as the standard says: the script ends early (and its stray end tag is dropped, so
    // the b stays open), the comment is truncated (its rest is text), the textarea shows its comment as text, the ul
    // closes the p.
    EXPECT_EQ(children(back.fragment),
        "<script></script><b>x<div><!--a-->b--&gt;</div><textarea>&lt;!--c--&gt;</textarea><p></p><ul></ul><p></p></b>");
  }
}

} // namespace
