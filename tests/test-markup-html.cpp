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
 * The HTML vocabulary (markup_html.h).
 *
 * - Every name the tables hold, looked up, with every flag it should and should
 *   not have; names next to them (prefixes, suffixes, case) that must miss.
 *   Looking up every entry is what shows the tables are in the order the
 *   binary search needs.
 * - Only the XHTML namespace answers: the same names in SVG, MathML, another
 *   namespace and no namespace do not.
 * - Boolean attributes: per element, global ones, and the near misses.
 * - Node variants on parsed XHTML, with no extra step.
 * - The table is checked against data it was not made from: the web-platform-
 *   tests tree-construction vectors and the html5lib serializer vectors. Absent
 *   data fails the test.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

namespace {

#define XHTML "http://www.w3.org/1999/xhtml"

const std::set<std::string> kVoid = {"area", "base", "basefont", "bgsound", "br", "col", "embed",
    "frame", "hr", "img", "input", "keygen", "link", "meta", "param", "source", "track", "wbr"};
const std::set<std::string> kRaw = {"script", "style"};
const std::set<std::string> kEscapable = {"textarea", "title"};
const std::set<std::string> kNewline = {"pre", "textarea", "listing"};

std::set<std::string> everything() {
  std::set<std::string> all;
  for (const auto * s : {&kVoid, &kRaw, &kEscapable, &kNewline}) {
    all.insert(s->begin(), s->end());
  }
  return all;
}

// A pin of the transcription (see the independent tests below for the standard).
TEST(MarkupHtml, EveryNameHasExactlyTheFlagsItShould) {
  for (const std::string & name : everything()) {
    SCOPED_TRACE(name);
    EXPECT_EQ(gtext_markup_html_is_void(XHTML, name.c_str()), kVoid.count(name) == 1);
    EXPECT_EQ(gtext_markup_html_is_raw_text(XHTML, name.c_str()), kRaw.count(name) == 1);
    EXPECT_EQ(gtext_markup_html_is_escapable_raw_text(XHTML, name.c_str()), kEscapable.count(name) == 1);
    EXPECT_EQ(gtext_markup_html_drops_first_newline(XHTML, name.c_str()), kNewline.count(name) == 1);
    const unsigned f = gtext_markup_html_element_flags(XHTML, name.c_str());
    EXPECT_NE(f, 0u);
    EXPECT_EQ((f & GTEXT_MARKUP_HTML_VOID) != 0, kVoid.count(name) == 1);
  }
  // The standard's 13, which are the ones that matter most.
  for (const char * n : {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
           "source", "track", "wbr"}) {
    EXPECT_TRUE(gtext_markup_html_is_void(XHTML, n)) << n;
  }
}

TEST(MarkupHtml, NamesNextToTheTableMiss) {
  const std::set<std::string> known = everything();
  const std::vector<std::string> others = {"", "a", "b", "div", "p", "span", "table", "td", "svg",
      "math", "template", "iframe", "noscript", "noembed", "xmp", "plaintext", "object", "image",
      "BR", "Br", "IMG", "SCRIPT", "Script", "Pre", "br ", " br", "br/", "b", "brr", "im", "imgs",
      "scripts", "scrip", "tit", "titles", "text", "textareas", "pr", "pree", "listin", "listings",
      "wb", "wbrr", "a-b", "x-br", "html", "head", "body"};
  for (const std::string & name : others) {
    SCOPED_TRACE(name);
    if (known.count(name)) {
      continue;
    }
    EXPECT_EQ(gtext_markup_html_element_flags(XHTML, name.c_str()), 0u);
    EXPECT_FALSE(gtext_markup_html_is_void(XHTML, name.c_str()));
    EXPECT_FALSE(gtext_markup_html_is_raw_text(XHTML, name.c_str()));
    EXPECT_FALSE(gtext_markup_html_is_escapable_raw_text(XHTML, name.c_str()));
    EXPECT_FALSE(gtext_markup_html_drops_first_newline(XHTML, name.c_str()));
  }
}

TEST(MarkupHtml, OnlyTheXhtmlNamespaceAnswers) {
  for (const std::string & name : everything()) {
    SCOPED_TRACE(name);
    for (const char * ns : {"http://www.w3.org/2000/svg", "http://www.w3.org/1998/Math/MathML",
             "http://www.w3.org/1999/xhtml/", "HTTP://WWW.W3.ORG/1999/XHTML", "", "urn:x",
             "http://www.w3.org/1999/xhtm", (const char *)nullptr}) {
      EXPECT_EQ(gtext_markup_html_element_flags(ns, name.c_str()), 0u) << (ns ? ns : "(null)");
      EXPECT_FALSE(gtext_markup_html_is_void(ns, name.c_str()));
      EXPECT_FALSE(gtext_markup_html_is_boolean_attribute(ns, name.c_str(), "hidden"));
    }
  }
  EXPECT_EQ(gtext_markup_html_element_flags(XHTML, nullptr), 0u);
  EXPECT_FALSE(gtext_markup_html_is_boolean_attribute(XHTML, nullptr, "hidden"));
  EXPECT_FALSE(gtext_markup_html_is_boolean_attribute(XHTML, "input", nullptr));
}

TEST(MarkupHtml, BooleanAttributesBelongToTheirElements) {
  struct Case {
    const char * element;
    const char * attribute;
    bool boolean;
  };
  const Case cases[] = {
      {"input", "disabled", true}, {"button", "disabled", true}, {"fieldset", "disabled", true},
      {"link", "disabled", true}, {"p", "disabled", false}, {"div", "disabled", false},
      {"input", "checked", true}, {"input", "required", true}, {"select", "required", true},
      {"textarea", "readonly", true}, {"select", "readonly", false}, {"option", "selected", true},
      {"input", "selected", false}, {"script", "async", true}, {"script", "defer", true},
      {"script", "nomodule", true}, {"div", "async", false}, {"video", "controls", true},
      {"audio", "loop", true}, {"video", "playsinline", true}, {"audio", "playsinline", false},
      {"details", "open", true}, {"dialog", "open", true}, {"div", "open", false},
      {"ol", "reversed", true}, {"ul", "reversed", false}, {"form", "novalidate", true},
      {"img", "ismap", true}, {"track", "default", true}, {"iframe", "allowfullscreen", true},
      {"template", "shadowrootclonable", true}, {"input", "multiple", true},
      {"select", "multiple", true}, {"textarea", "multiple", false},
      // The global ones, on any element, known or not.
      {"div", "autofocus", true}, {"span", "inert", true}, {"anything", "itemscope", true},
      {"p", "headingreset", true},
      // Near misses.
      {"input", "disable", false}, {"input", "disabledd", false}, {"input", "DISABLED", false},
      {"input", "", false}, {"input", "value", false}, {"div", "hidden", false},
      {"div", "class", false}, {"button", "formnovalidate", true}, {"button", "form", false},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.element) + " " + c.attribute);
    EXPECT_EQ(gtext_markup_html_is_boolean_attribute(XHTML, c.element, c.attribute), c.boolean);
  }
}

GTEXT_Markup_Document * parse(const std::string & text) {
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_XML_Error err;
  const GTEXT_XML_Status st =
      gtext_xml_parse(text.data(), text.size(), nullptr, &doc, &err);
  EXPECT_EQ(st, GTEXT_XML_OK) << (err.message ? err.message : "");
  return st == GTEXT_XML_OK ? doc : nullptr;
}

TEST(MarkupHtml, ParsedXhtmlAnswersWithNoExtraStep) {
  GTEXT_Markup_Document * doc = parse(
      "<html xmlns=\"" XHTML "\"><body><br/><pre>x</pre><script>1</script>"
      "<textarea></textarea><input disabled=\"disabled\"/><p>t</p>"
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><br/><script/></svg></body></html>");
  ASSERT_NE(doc, nullptr);
  const GTEXT_Markup_Node * body = gtext_markup_node_first_child(gtext_markup_document_element(doc));
  const GTEXT_Markup_Node * br = gtext_markup_node_first_child(body);
  const GTEXT_Markup_Node * pre = gtext_markup_node_next_sibling(br);
  const GTEXT_Markup_Node * script = gtext_markup_node_next_sibling(pre);
  const GTEXT_Markup_Node * textarea = gtext_markup_node_next_sibling(script);
  const GTEXT_Markup_Node * input = gtext_markup_node_next_sibling(textarea);
  const GTEXT_Markup_Node * p = gtext_markup_node_next_sibling(input);
  const GTEXT_Markup_Node * svg = gtext_markup_node_next_sibling(p);
  EXPECT_TRUE(gtext_markup_html_node_is_void(br));
  EXPECT_FALSE(gtext_markup_html_node_is_void(pre));
  EXPECT_TRUE(gtext_markup_html_node_drops_first_newline(pre));
  EXPECT_TRUE(gtext_markup_html_node_is_raw_text(script));
  EXPECT_FALSE(gtext_markup_html_node_is_escapable_raw_text(script));
  EXPECT_TRUE(gtext_markup_html_node_is_escapable_raw_text(textarea));
  EXPECT_TRUE(gtext_markup_html_node_drops_first_newline(textarea));
  EXPECT_TRUE(gtext_markup_html_node_is_boolean_attribute(input, "disabled"));
  EXPECT_FALSE(gtext_markup_html_node_is_boolean_attribute(p, "disabled"));
  EXPECT_EQ(gtext_markup_html_node_flags(p), 0u);
  // The same names in the SVG namespace are not HTML.
  const GTEXT_Markup_Node * svg_br = gtext_markup_node_first_child(svg);
  const GTEXT_Markup_Node * svg_script = gtext_markup_node_next_sibling(svg_br);
  EXPECT_FALSE(gtext_markup_html_node_is_void(svg_br));
  EXPECT_FALSE(gtext_markup_html_node_is_raw_text(svg_script));
  // Not an element, and NULL.
  EXPECT_EQ(gtext_markup_html_node_flags(gtext_markup_node_first_child(p)), 0u);
  EXPECT_EQ(gtext_markup_html_node_flags(nullptr), 0u);
  EXPECT_FALSE(gtext_markup_html_node_is_void(nullptr));
  EXPECT_FALSE(gtext_markup_html_node_is_boolean_attribute(nullptr, "disabled"));
  EXPECT_FALSE(gtext_markup_html_node_is_boolean_attribute(input, nullptr));
  gtext_markup_document_free(doc);
}

// ---- independent evidence ----------------------------------------------------
//
// The sets above are the table transcribed a second time: they pin the
// transcription and prove nothing about the standard. What does is data this
// code did not write, pinned by tools/html/fetch.sh:
//   * the web-platform-tests tree-construction vectors: the tree the standard's
//     parser builds. An HTML element that has a child there is not void, and a
//     raw-text or escapable-raw-text element never has an element child (its
//     content is text).
//   * the html5lib serializer vectors: which elements are written `<img />` in
//     their XHTML form, and which attributes their serializer minimizes.
// Absent data is a FAILURE, not a skip: this test would otherwise report green
// having compared nothing (`make test` runs the check-xhtml-test-data gate first,
// which says how to fetch it).
// What this cannot show is that the table is complete: a void element the table
// lacks and the corpus has no test for would pass. The WHATWG lists (13.1.2,
// retrieved 2026-10-11) are the completeness source, by hand.

struct Seen {
  size_t seen = 0;
  size_t with_children = 0;
  size_t with_element_child = 0;
};

bool ends_with(const std::string & s, const char * suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Reads the "#document" trees of one .dat file.
void scan_dat(const std::string & path, std::map<std::string, Seen> & out) {
  std::ifstream in(path);
  std::string line;
  bool in_doc = false;
  std::vector<std::pair<int, std::string>> nodes;  // depth, text (without attributes)
  std::vector<std::string> lines;
  auto flush = [&]() {
    // Each element line, then the next non-attribute line, give a child or not.
    struct Node {
      int depth;
      std::string text;
    };
    std::vector<Node> v;
    for (const std::string & l : lines) {
      if (l.size() < 2 || l[0] != '|') {
        continue;
      }
      size_t i = 2;
      int spaces = 0;
      while (i < l.size() && l[i] == ' ') {
        i++;
        spaces++;
      }
      const std::string body = l.substr(i);
      const bool node = (!body.empty() && body[0] == '<' && body.back() == '>')
          || (!body.empty() && body[0] == '"') || body == "content";
      if (node) {
        v.push_back({spaces / 2, body});
      }
    }
    for (size_t k = 0; k < v.size(); k++) {
      const std::string & t = v[k].text;
      if (t.size() < 3 || t[0] != '<' || t[1] == '!' || t.find(' ') != std::string::npos) {
        continue;  // not an HTML element line (comments, doctypes, svg/math)
      }
      const std::string name = t.substr(1, t.size() - 2);
      Seen & st = out[name];
      st.seen++;
      // Children are the following lines that are deeper.
      for (size_t j = k + 1; j < v.size() && v[j].depth > v[k].depth; j++) {
        st.with_children++;
        break;
      }
      for (size_t j = k + 1; j < v.size() && v[j].depth > v[k].depth; j++) {
        if (v[j].depth == v[k].depth + 1 && v[j].text[0] == '<' && v[j].text[1] != '!') {
          st.with_element_child++;
          break;
        }
      }
    }
    lines.clear();
  };
  while (std::getline(in, line)) {
    if (line == "#data") {
      if (in_doc) {
        flush();
      }
      in_doc = false;
    }
    else if (line == "#document") {
      in_doc = true;
      lines.clear();
    }
    else if (in_doc && !line.empty() && line[0] == '|') {
      lines.push_back(line);
    }
    else if (in_doc && line.empty()) {
      flush();
      in_doc = false;
    }
  }
  if (in_doc) {
    flush();
  }
}

TEST(MarkupHtml, TheTableAgreesWithTheTreesTheStandardsParserBuilds) {
  const std::string dir = "third_party/html/wpt/html/syntax/parsing/resources";
  DIR * d = opendir(dir.c_str());
  ASSERT_NE(d, nullptr) << dir << " is not fetched: run tools/html/fetch.sh";
  std::map<std::string, Seen> seen;
  size_t files = 0;
  while (dirent * e = readdir(d)) {
    const std::string name = e->d_name;
    if (ends_with(name, ".dat")) {
      scan_dat(dir + "/" + name, seen);
      files++;
    }
  }
  closedir(d);
  ASSERT_GT(files, 20u);
  ASSERT_GT(seen.size(), 60u);
  size_t void_checked = 0;
  for (const auto & [name, st] : seen) {
    SCOPED_TRACE(name);
    if (st.with_children > 0) {
      EXPECT_FALSE(gtext_markup_html_is_void(XHTML, name.c_str()))
          << "has children in a tree the standard's parser builds";
    }
  }
  for (const std::string & name : kVoid) {
    SCOPED_TRACE(name);
    ASSERT_EQ(seen.count(name), 1u) << "the corpus never builds this void element";
    EXPECT_EQ(seen[name].with_children, 0u);
    void_checked++;
  }
  EXPECT_EQ(void_checked, kVoid.size());
  for (const std::set<std::string> * group : {&kRaw, &kEscapable}) {
    for (const std::string & name : *group) {
      SCOPED_TRACE(name);
      ASSERT_EQ(seen.count(name), 1u);
      EXPECT_EQ(seen[name].with_element_child, 0u) << "content is text";
    }
  }
  // And the converse is informative, not asserted: elements with no children
  // anywhere in the corpus that the table does not call void are just empty
  // elements. What would be a defect is one the corpus shows as childless in
  // every one of many appearances and the standard lists as void.
}

TEST(MarkupHtml, TheSerializerVectorsAgreeOnVoidAndBooleanAttributes) {
  std::ifstream core("third_party/html/html5lib-tests/serializer/core.test");
  std::ifstream options("third_party/html/html5lib-tests/serializer/options.test");
  ASSERT_TRUE(core && options) << "third_party/html/html5lib-tests is not fetched: run tools/html/fetch.sh";
  std::stringstream cs, os;
  cs << core.rdbuf();
  os << options.rdbuf();
  const std::string core_text = cs.str();
  const std::string options_text = os.str();
  // Every vector whose XHTML form is `<name />` names a void element.
  size_t void_vectors = 0;
  for (size_t at = 0; (at = core_text.find("\"xhtml\":", at)) != std::string::npos; at++) {
    const size_t end = core_text.find('\n', at);
    const std::string row = core_text.substr(at, end - at);
    const size_t lt = row.find("[\"<");
    const size_t sp = row.find(" />\"]");
    if (lt == std::string::npos || sp == std::string::npos) {
      continue;
    }
    const std::string name = row.substr(lt + 3, sp - lt - 3);
    EXPECT_TRUE(gtext_markup_html_is_void(XHTML, name.c_str())) << name;
    void_vectors++;
  }
  EXPECT_GE(void_vectors, 2u);
  // html5lib minimizes `disabled` on a button and has no such attribute on a
  // div (`irrelevant`): the vectors that say so exist, and the table agrees.
  EXPECT_NE(options_text.find("\"button\", [{\"namespace\": null, \"name\": \"disabled\""), std::string::npos);
  EXPECT_NE(options_text.find("\"expected\": [\"<button disabled>\"]"), std::string::npos);
  EXPECT_TRUE(gtext_markup_html_is_boolean_attribute(XHTML, "button", "disabled"));
  EXPECT_NE(options_text.find("\"div\", [{\"namespace\": null, \"name\": \"irrelevant\""), std::string::npos);
  EXPECT_FALSE(gtext_markup_html_is_boolean_attribute(XHTML, "div", "irrelevant"));
}

// The names an HTML parser restores in foreign content (markup_html.c cites where the
// tables are from). A pin of the transcription, as for the element tables above: every
// listed name survives, its lower case form survives (it is the form HTML reads), and the
// near misses do not.
TEST(MarkupHtml, ForeignNamesSurviveWhenHtmlRestoresTheirCase) {
  const char * svg = "http://www.w3.org/2000/svg";
  const char * math = "http://www.w3.org/1998/Math/MathML";
  const char * const tags[] = {"altGlyph", "altGlyphDef", "altGlyphItem", "animateColor", "animateMotion",
      "animateTransform", "clipPath", "feBlend", "feColorMatrix", "feComponentTransfer", "feComposite",
      "feConvolveMatrix", "feDiffuseLighting", "feDisplacementMap", "feDistantLight", "feFlood", "feFuncA",
      "feFuncB", "feFuncG", "feFuncR", "feGaussianBlur", "feImage", "feMerge", "feMergeNode", "feMorphology",
      "feOffset", "fePointLight", "feSpecularLighting", "feSpotLight", "feTile", "feTurbulence",
      "foreignObject", "glyphRef", "linearGradient", "radialGradient", "textPath"};
  const char * const attrs[] = {"attributeName", "attributeType", "baseFrequency", "baseProfile", "calcMode",
      "clipPathUnits", "diffuseConstant", "edgeMode", "filterUnits", "glyphRef", "gradientTransform",
      "gradientUnits", "kernelMatrix", "kernelUnitLength", "keyPoints", "keySplines", "keyTimes",
      "lengthAdjust", "limitingConeAngle", "markerHeight", "markerUnits", "markerWidth", "maskContentUnits",
      "maskUnits", "numOctaves", "pathLength", "patternContentUnits", "patternTransform", "patternUnits",
      "pointsAtX", "pointsAtY", "pointsAtZ", "preserveAlpha", "preserveAspectRatio", "primitiveUnits", "refX",
      "refY", "repeatCount", "repeatDur", "requiredExtensions", "requiredFeatures", "specularConstant",
      "specularExponent", "spreadMethod", "startOffset", "stdDeviation", "stitchTiles", "surfaceScale",
      "systemLanguage", "tableValues", "targetX", "targetY", "textLength", "viewBox", "viewTarget",
      "xChannelSelector", "yChannelSelector", "zoomAndPan"};
  for (const char * t : tags) {
    SCOPED_TRACE(t);
    EXPECT_TRUE(gtext_markup_html_foreign_name_survives(svg, false, t));
    EXPECT_EQ(gtext_markup_html_foreign_name_survives(svg, true, t), std::strcmp(t, "glyphRef") == 0);
    EXPECT_FALSE(gtext_markup_html_foreign_name_survives(math, false, t));
    std::string lower(t);
    for (char & ch : lower) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    // The folded form of an entry is restored to the entry, so it is not read back as written.
    EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, false, lower.c_str()));
    EXPECT_TRUE(gtext_markup_html_foreign_name_survives(math, false, lower.c_str()));
    EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, false, (std::string(t) + "X").c_str()));
  }
  for (const char * a : attrs) {
    SCOPED_TRACE(a);
    EXPECT_TRUE(gtext_markup_html_foreign_name_survives(svg, true, a));
    EXPECT_FALSE(gtext_markup_html_foreign_name_survives(math, true, a));
    EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, true, (std::string(a) + "X").c_str()));
    EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, true, (std::string("X") + a).c_str()));
    std::string folded(a);
    for (char & ch : folded) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, true, folded.c_str()));
    EXPECT_TRUE(gtext_markup_html_foreign_name_survives(math, true, folded.c_str()));
  }
  EXPECT_TRUE(gtext_markup_html_foreign_name_survives(math, true, "definitionURL"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(math, true, "definitionurl"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, true, "definitionURL"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(math, false, "definitionURL"));
  // Dropped by the standard: read lower case, so not as written.
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, true, "contentScriptType"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, true, "contentStyleType"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, false, "customElement"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(math, false, "Foo"));
  // No upper case: read as written, in these two namespaces only.
  EXPECT_TRUE(gtext_markup_html_foreign_name_survives(svg, false, "g"));
  EXPECT_TRUE(gtext_markup_html_foreign_name_survives(math, true, "mathvariant"));
  EXPECT_TRUE(gtext_markup_html_foreign_name_survives(svg, true, "data-x"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(XHTML, false, "p"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(nullptr, false, "g"));
  EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, false, nullptr));
}

}  // namespace
