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
 * The computed-style call (css_style.h) and the presentation attributes (css_presentation.h): the requirements of the specification of phase C2d,
 * one test each, the SVG fixtures of tests/data/css/svg/ computed and compared with their expected dumps byte for byte, and the dump and the hash.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/security/sha256.h>
#include <ghoti.io/text/css.h>

#include "css_compute_test_support.h"
#include "css_markup_tree.h"
#include "css_svg_registry.h"

namespace {

using cmp::find;


const GTEXT_CSS_Registry * R() {
  return svgreg::shared();
}

const char * const kSvg = GTEXT_CSS_SVG_NAMESPACE;

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string find_file(const std::string & relative_to_tests) {
  std::vector<std::string> roots;
  const std::string here = __FILE__;
  const size_t slash = here.rfind('/');
  roots.push_back(slash == std::string::npos ? "." : here.substr(0, slash));
  roots.push_back("tests");
  roots.push_back("../..");
  for (const std::string & r : roots) {
    const std::string path = r + "/" + relative_to_tests;
    std::ifstream in(path, std::ios::binary);
    if (in) {
      return path;
    }
  }
  return "";
}

std::string hex(const unsigned char * d, size_t n) {
  static const char * digits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += digits[d[i] >> 4];
    s += digits[d[i] & 15];
  }
  return s;
}

/// A document, the context over it and the style of every element.
struct Doc {
  cmt::Tree tree;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  GTEXT_CSS_Compute_Result result{};
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;

  Doc(const std::string & text, bool html = false, const GTEXT_CSS_Style_Options * options = nullptr, const GTEXT_CSS_Environment * env = nullptr,
      const GTEXT_CSS_Substitute_Options * limits = nullptr) {
    EXPECT_TRUE(tree.parse(text, html)) << text;
    tree.build();
    GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
    co.elements = &tree.table;
    if (limits) {
      co.substitute = *limits;
    }
    GTEXT_CSS_Status st = gtext_css_compute_context_new(R(), env, &co, &ctx, &result);
    EXPECT_EQ(st, GTEXT_CSS_OK);
    status = tree.compute_all(ctx, options, &result);
  }
  Doc(const Doc &) = delete;
  Doc & operator=(const Doc &) = delete;
  ~Doc() {
    for (auto & i : tree.all) {
      gtext_css_computed_free(i->style);
      i->style = nullptr;
    }
    gtext_css_compute_context_free(ctx);
  }
  cmt::Inst * at(const std::string & path) {
    cmt::Inst * i = tree.find(path);
    EXPECT_NE(i, nullptr) << path;
    return i;
  }
  /// The computed value of a property of an element, serialized.
  std::string value(const std::string & path, const std::string & property) {
    cmt::Inst * i = at(path);
    if (!i || !i->style) {
      return "<no style>";
    }
    const GTEXT_CSS_Value_Set * set = nullptr;
    const GTEXT_CSS_Status st = gtext_css_computed_get_by_name(i->style, property.data(), property.size(), &set, nullptr);
    if (st != GTEXT_CSS_OK) {
      return std::string("<") + gtext_css_status_string(st) + ">";
    }
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
    if (!root) {
      return "";
    }
    char * out = nullptr;
    size_t n = 0;
    if (gtext_css_serialize_value_buffer(root, nullptr, &out, &n) != GTEXT_CSS_OK) {
      return "<serialize error>";
    }
    std::string text(out, n);
    gtext_css_serialize_buffer_free(nullptr, out);
    return text;
  }
};

std::string svg(const std::string & inner, const std::string & root_attrs = "") {
  return std::string("<svg xmlns=\"") + kSvg + "\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" id=\"root\" " + root_attrs + ">" + inner + "</svg>";
}

std::string dump_of(const GTEXT_CSS_Computed_Style * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
    static_cast<std::string *>(u)->append(b, n);
    return GTEXT_MARKUP_OK;
  };
  EXPECT_EQ(gtext_css_computed_dump(s, &sink), GTEXT_CSS_OK);
  return out;
}

/* ---- the matrix of the specification ------------------------------------------------------------------------------------ */

TEST(CssStyleRequirements, AnAttributeIsAPropertyOfTheElement) {
  /* | Attribute | `<rect stroke-width="2">`, no style | `stroke-width: 2px` computed | */
  Doc d(svg("<rect id=\"r\" stroke-width=\"2\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("r", "stroke-width"), "2px");
  EXPECT_EQ(d.value("root", "stroke-width"), "1px") << "the initial value, where nothing is written";
}

TEST(CssStyleRequirements, TheStyleAttributeBeatsThePresentationAttribute) {
  /* | Style wins | the same plus `style="stroke-width:5"` | `5px` | */
  Doc d(svg("<rect id=\"r\" stroke-width=\"2\" style=\"stroke-width:5\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.value("r", "stroke-width"), "5px");
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_computed_specified(d.at("r")->style, find(R(), "stroke-width"), &e));
  EXPECT_TRUE(e.element_attached);
  EXPECT_FALSE(e.presentational_hint);
}

TEST(CssStyleRequirements, AnAuthorRuleBeatsAPresentationAttribute) {
  /* | Hint loses | presentation attribute vs an author rule entry | the rule wins | */
  Doc d(svg("<rect id=\"r\" stroke-width=\"2\" fill=\"red\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  cmt::Inst * r = d.at("r");
  GTEXT_CSS_Style_Entry entry;
  memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  entry.text = {"stroke-width: 7px", 17};
  GTEXT_CSS_Computed_Style * s = nullptr;
  ASSERT_EQ(gtext_css_style_compute(d.ctx, r, nullptr, &entry, 1, nullptr, &s, nullptr), GTEXT_CSS_OK);
  const GTEXT_CSS_Value * v = gtext_css_computed_root(s, find(R(), "stroke-width"));
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_serialize_value_buffer(v, nullptr, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "7px");
  gtext_css_serialize_buffer_free(nullptr, out);
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_computed_specified(s, find(R(), "stroke-width"), &e));
  EXPECT_FALSE(e.presentational_hint) << "the rule's declaration is the specified value";
  ASSERT_TRUE(gtext_css_computed_specified(s, find(R(), "fill"), &e));
  EXPECT_TRUE(e.presentational_hint) << "what no rule sets is the attribute's";
  gtext_css_computed_free(s);
}

TEST(CssStyleRequirements, ABadAttributeIsDroppedWithADiagnosticAndTheValueIsInherited) {
  /* | Bad attribute | `fill="12px"` | dropped, `fill` inherited or initial | diagnostic with offset into the value | */
  Doc d(svg("<g id=\"g\" fill=\"orange\"><rect id=\"r\" fill=\"12px\"/></g><rect id=\"alone\" fill=\"12px\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.value("r", "fill"), "orange");
  EXPECT_EQ(d.value("alone", "fill"), "black") << "initial";
  GTEXT_CSS_Style_Diagnostic diag;
  ASSERT_EQ(gtext_css_computed_diagnostic_total(d.at("r")->style), 1u);
  ASSERT_TRUE(gtext_css_computed_diagnostic(d.at("r")->style, 0, &diag));
  EXPECT_EQ(diag.kind, GTEXT_CSS_STYLE_DIAG_INVALID_VALUE);
  EXPECT_EQ(diag.source, GTEXT_CSS_STYLE_SOURCE_PRESENTATION);
  EXPECT_EQ(std::string(diag.name.data, diag.name.length), "fill");
  EXPECT_EQ(diag.offset, 0u) << "the place in the attribute's value";
  EXPECT_FALSE(gtext_css_computed_diagnostic(d.at("r")->style, 1, &diag));
}

TEST(CssStyleRequirements, ADiagnosticIsAnOffsetIntoTheValuePlusTheBase) {
  Doc d(svg("<rect id=\"r\" fill=\"  12px \"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  GTEXT_CSS_Style_Diagnostic diag;
  ASSERT_TRUE(gtext_css_computed_diagnostic(d.at("r")->style, 0, &diag));
  EXPECT_EQ(diag.offset, 2u) << "white space at the ends is not the value";
  GTEXT_CSS_Style_Options o = gtext_css_style_options_default();
  o.presentation.base_offset = 100;
  Doc e(svg("<rect id=\"r\" fill=\"  12px \"/>"), false, &o);
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_TRUE(gtext_css_computed_diagnostic(e.at("r")->style, 0, &diag));
  EXPECT_EQ(diag.offset, 102u) << "plus where the value begins in the caller's document";
  o.presentation.trim_whitespace = false;
  Doc f(svg("<rect id=\"r\" fill=\"  12px \"/>"), false, &o);
  ASSERT_TRUE(gtext_css_computed_diagnostic(f.at("r")->style, 0, &diag));
  EXPECT_EQ(diag.offset, 100u) << "without the option the white space is part of the value";
}

TEST(CssStyleRequirements, ACloneOfAUseInheritsFromTheUseAndNotFromItsTreeParent) {
  /* | `<use>` | clone's `inheritance_parent` is the `<use>` | inherits from it, not its tree parent | */
  Doc d(svg("<defs><g id=\"src\" fill=\"red\"><rect id=\"inner\"/></g></defs><g fill=\"green\"><use id=\"u\" href=\"#inner\" fill=\"yellow\"/></g>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("inner", "fill"), "red") << "the original inherits from its group";
  cmt::Inst * clone = d.at("u>inner");
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(clone->inherit, d.at("u"));
  EXPECT_NE(clone->parent, clone->inherit) << "the tree parent is where the original is";
  EXPECT_EQ(d.value("u>inner", "fill"), "yellow") << "from the use";
}

TEST(CssStyleRequirements, TwoCascadesWithEqualResultsHaveTheSameDumpAndHash) {
  /* | Dump | two cascades with equal results | identical dump and hash | */
  Doc d(svg("<rect id=\"a\" fill=\"red\" stroke-width=\"2\"/>"
            "<rect id=\"b\" style=\"stroke-width: 2px; fill: red\"/>"
            "<rect id=\"c\" fill=\"blue\" style=\"fill: red\" stroke-width=\"1\" /><rect id=\"d\" fill=\"red\" stroke-width=\"3\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  const std::string a = dump_of(d.at("a")->style), b = dump_of(d.at("b")->style), c = dump_of(d.at("c")->style);
  EXPECT_EQ(a, b) << "where a value comes from is not in the dump";
  unsigned char ha[32], hb[32], hc[32];
  ASSERT_EQ(gtext_css_computed_hash(d.at("a")->style, ha), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_computed_hash(d.at("b")->style, hb), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_computed_hash(d.at("c")->style, hc), GTEXT_CSS_OK);
  EXPECT_EQ(memcmp(ha, hb, 32), 0);
  EXPECT_NE(memcmp(ha, hc, 32), 0) << "a different stroke-width";
  EXPECT_NE(a, c);
}

TEST(CssStyleRequirements, APropertyTheEngineCannotComputeIsUnsupportedAndNamed) {
  /* | Unsupported | a property outside the computed set | none | `E_UNSUPPORTED` naming it | */
  Doc d(svg("<rect id=\"r\" style=\"corner-top-left-shape: round\"/>"));
  EXPECT_EQ(d.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(std::string(d.result.name), "corner-top-left-shape");
  Doc e(svg("<rect id=\"r\"/>"));
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  GTEXT_CSS_Compute_Result res;
  const GTEXT_CSS_Value_Set * set = nullptr;
  const GTEXT_CSS_Status st = gtext_css_computed_get(e.at("r")->style, find(R(), "corner-top-left-shape"), &set, &res);
  /* it has an initial value or it has none: when it has none the read names it */
  if (st == GTEXT_CSS_E_UNSUPPORTED) {
    EXPECT_EQ(std::string(res.name), "corner-top-left-shape");
  }
  else {
    EXPECT_EQ(st, GTEXT_CSS_OK);
  }
}

/* ---- the call ----------------------------------------------------------------------------------------------------------- */

TEST(CssStyleCall, ArgumentsAreChecked) {
  Doc d(svg("<rect id=\"r\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  cmt::Inst * r = d.at("r");
  GTEXT_CSS_Computed_Style * s = nullptr;
  GTEXT_CSS_Compute_Result res;
  EXPECT_EQ(gtext_css_style_compute(nullptr, r, nullptr, nullptr, 0, nullptr, &s, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_style_compute(d.ctx, r, nullptr, nullptr, 0, nullptr, nullptr, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_style_compute(d.ctx, r, nullptr, nullptr, 3, nullptr, &s, &res), GTEXT_CSS_E_INVALID) << "entries are counted and absent";
  EXPECT_EQ(gtext_css_style_compute(d.ctx, nullptr, nullptr, nullptr, 0, nullptr, &s, &res), GTEXT_CSS_E_INVALID) << "attributes are read, and there is no element";
  EXPECT_EQ(s, nullptr);
  /* a parent of another context */
  GTEXT_CSS_Compute_Context * other = nullptr;
  ASSERT_EQ(gtext_css_compute_context_new(R(), nullptr, nullptr, &other, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Style_Options none;
  memset(&none, 0, sizeof none);
  EXPECT_EQ(gtext_css_style_compute(other, nullptr, d.at("root")->style, nullptr, 0, &none, &s, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(s, nullptr);
  /* reading attributes needs an element interface */
  EXPECT_EQ(gtext_css_style_compute(other, r, nullptr, nullptr, 0, nullptr, &s, &res), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(std::string(res.name), "attribute");
  /* ... and with no sources the entries alone are enough */
  EXPECT_EQ(gtext_css_style_compute(other, nullptr, nullptr, nullptr, 0, &none, &s, &res), GTEXT_CSS_OK);
  gtext_css_computed_free(s);
  gtext_css_compute_context_free(other);
}

TEST(CssStyleCall, AMissingCallbackIsNamed) {
  /* an interface with no `namespace_uri` cannot tell an SVG element from another */
  Doc d(svg("<rect id=\"r\" fill=\"red\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  GTEXT_CSS_Element_Interface iface = d.tree.table;
  iface.namespace_uri = nullptr;
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = &iface;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  ASSERT_EQ(gtext_css_compute_context_new(R(), nullptr, &co, &ctx, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Computed_Style * s = nullptr;
  GTEXT_CSS_Compute_Result res;
  EXPECT_EQ(gtext_css_style_compute(ctx, d.at("r"), nullptr, nullptr, 0, nullptr, &s, &res), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(std::string(res.name), "namespace_uri");
  GTEXT_CSS_Style_Options only_style = gtext_css_style_options_default();
  only_style.sources = GTEXT_CSS_STYLE_STYLE_ATTRIBUTE;
  EXPECT_EQ(gtext_css_style_compute(ctx, d.at("r"), nullptr, nullptr, 0, &only_style, &s, &res), GTEXT_CSS_OK) << "the style attribute does not need it";
  gtext_css_computed_free(s);
  gtext_css_compute_context_free(ctx);
}

TEST(CssStyleCall, AStyleIsReferenceCountedAndOutlivesItsOwnersOneByOne) {
  Doc d(svg("<rect id=\"r\" fill=\"red\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  GTEXT_CSS_Computed_Style * s = d.at("r")->style;
  GTEXT_CSS_Computed_Style * t = gtext_css_computed_retain(s);
  EXPECT_EQ(t, s);
  EXPECT_EQ(gtext_css_computed_retain(nullptr), nullptr);
  gtext_css_computed_free(t);
  EXPECT_EQ(d.value("r", "fill"), "red") << "one owner is still here";
}

TEST(CssStyleCall, ReadsByNameInRegistryOrderAndWithTheCustomProperties) {
  Doc d(svg("<rect id=\"r\" style=\"--k: 1; fill: red\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  cmt::Inst * r = d.at("r");
  const GTEXT_CSS_Value_Set * set = nullptr;
  EXPECT_EQ(gtext_css_computed_get_by_name(r->style, "FILL", 4, &set, nullptr), GTEXT_CSS_OK) << "property names are ASCII case-insensitive";
  EXPECT_EQ(gtext_css_computed_get_by_name(r->style, "--k", 3, &set, nullptr), GTEXT_CSS_E_INVALID) << "a custom property has its own read";
  EXPECT_EQ(gtext_css_computed_get_by_name(r->style, "no-such", 7, &set, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_get(r->style, find(R(), "margin"), &set, nullptr), GTEXT_CSS_E_INVALID) << "a shorthand has no value";
  const GTEXT_CSS_Custom_Value * cv = nullptr;
  EXPECT_TRUE(gtext_css_computed_custom_find(r->style, "--k", 3, &cv));
  size_t cursor = 0, count = 0;
  GTEXT_CSS_Property_Id id = 0, last = 0;
  while (gtext_css_computed_next(r->style, &cursor, &id, &set)) {
    EXPECT_GT(id, last);
    EXPECT_NE(set, nullptr);
    last = id;
    count++;
  }
  EXPECT_GT(count, 300u);
  EXPECT_FALSE(gtext_css_computed_next(r->style, &cursor, &id, &set)) << "the end stays the end";
}

TEST(CssStyleCall, TheSpecifiedValueHasItsProvenance) {
  Doc d(svg("<rect id=\"r\" fill=\"red\" stroke=\"blue\" style=\"fill: green !important; --k: 2\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  cmt::Inst * r = d.at("r");
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_computed_specified(r->style, find(R(), "fill"), &e));
  EXPECT_TRUE(e.important);
  EXPECT_TRUE(e.element_attached);
  EXPECT_EQ(e.origin, GTEXT_CSS_ORIGIN_AUTHOR);
  EXPECT_GT(e.span.end, e.span.offset);
  EXPECT_NE(e.set, nullptr);
  ASSERT_TRUE(gtext_css_computed_specified(r->style, find(R(), "stroke"), &e));
  EXPECT_TRUE(e.presentational_hint);
  EXPECT_FALSE(e.important);
  EXPECT_FALSE(gtext_css_computed_specified(r->style, find(R(), "opacity"), &e)) << "nothing declared it";
  EXPECT_FALSE(gtext_css_computed_specified(r->style, 0, &e));
  EXPECT_TRUE(gtext_css_computed_specified_custom(r->style, "--k", 3, &e));
  EXPECT_FALSE(gtext_css_computed_specified_custom(r->style, "--nope", 6, &e));
  EXPECT_NE(gtext_css_computed_cascaded(r->style), nullptr);
  EXPECT_FALSE(gtext_css_computed_specified(nullptr, find(R(), "fill"), &e));
  /* a style made from a block has no cascade */
  cmp::Context ctx(R());
  cmp::Block b(R(), "fill: red");
  cmp::Style s;
  s.compute(ctx, &b);
  EXPECT_FALSE(gtext_css_computed_specified(s.s, find(R(), "fill"), &e));
  EXPECT_EQ(gtext_css_computed_cascaded(s.s), nullptr);
  EXPECT_EQ(gtext_css_computed_diagnostic_total(s.s), 0u);
}

TEST(CssStyleCall, DroppedDeclarationsAreDiagnosticsAndTheRestStands) {
  Doc d(svg("<rect id=\"r\" style=\"fill: red; nonsense: 1; stroke-width: banana;\n  width: 3px\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  cmt::Inst * r = d.at("r");
  EXPECT_EQ(d.value("r", "fill"), "red");
  EXPECT_EQ(d.value("r", "width"), "3px");
  EXPECT_EQ(gtext_css_computed_diagnostic_total(r->style), 2u);
  GTEXT_CSS_Style_Diagnostic g;
  ASSERT_TRUE(gtext_css_computed_diagnostic(r->style, 0, &g));
  EXPECT_EQ(g.kind, GTEXT_CSS_STYLE_DIAG_UNKNOWN_PROPERTY);
  EXPECT_EQ(g.source, GTEXT_CSS_STYLE_SOURCE_STYLE_ATTRIBUTE);
  EXPECT_EQ(std::string(g.name.data, g.name.length), "nonsense");
  EXPECT_EQ(g.offset, 11u);
  EXPECT_EQ(g.line, 1u);
  ASSERT_TRUE(gtext_css_computed_diagnostic(r->style, 1, &g));
  EXPECT_EQ(g.kind, GTEXT_CSS_STYLE_DIAG_INVALID_VALUE);
  EXPECT_EQ(std::string(g.name.data, g.name.length), "stroke-width");
  GTEXT_CSS_Style_Options o = gtext_css_style_options_default();
  o.style_attribute_offset = 1000;
  Doc e(svg("<rect id=\"r\" style=\"x: 1\"/>"), false, &o);
  ASSERT_TRUE(gtext_css_computed_diagnostic(e.at("r")->style, 0, &g));
  EXPECT_EQ(g.offset, 1000u);
}

/* ---- entries and the cascade -------------------------------------------------------------------------------------------- */

struct Entries {
  std::vector<GTEXT_CSS_Style_Entry> list;
  std::vector<std::string> texts;
  void add(const std::string & text, uint32_t rule, unsigned specificity_b = 0) {
    texts.push_back(text);
    GTEXT_CSS_Style_Entry e;
    memset(&e, 0, sizeof e);
    e.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    e.keys.rule = rule;
    e.keys.specificity.b = specificity_b;
    list.push_back(e);
  }
  void fix() {
    for (size_t i = 0; i < list.size(); i++) {
      list[i].text = {texts[i].data(), texts[i].size()};
    }
  }
};

TEST(CssStyleEntries, TheOrderOfTheCascadeHoldsAcrossAttributesStyleAndRules) {
  Doc d(svg("<rect id=\"r\" fill=\"red\" opacity=\"0.1\" stroke-width=\"1\" style=\"stroke-width: 4px\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  Entries e;
  e.add("fill: green; stroke-width: 9px !important", 1);
  e.add("fill: blue", 2);
  e.add("fill: teal", 0, 1);
  e.fix();
  GTEXT_CSS_Computed_Style * s = nullptr;
  ASSERT_EQ(gtext_css_style_compute(d.ctx, d.at("r"), nullptr, e.list.data(), e.list.size(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  auto val = [&](const char * name) {
    const GTEXT_CSS_Value * v = gtext_css_computed_root(s, find(R(), name));
    char * out = nullptr;
    size_t n = 0;
    EXPECT_EQ(gtext_css_serialize_value_buffer(v, nullptr, &out, &n), GTEXT_CSS_OK);
    std::string t(out, n);
    gtext_css_serialize_buffer_free(nullptr, out);
    return t;
  };
  EXPECT_EQ(val("fill"), "teal") << "specificity first, then the order of appearance; the attribute is under all of them";
  EXPECT_EQ(val("stroke-width"), "9px") << "an important rule beats the style attribute's normal declaration";
  EXPECT_EQ(val("opacity"), "0.1") << "the attribute where no rule says";
  gtext_css_computed_free(s);
}

TEST(CssStyleEntries, TheImportantStyleAttributeBeatsTheImportantRule) {
  Doc d(svg("<rect id=\"r\" style=\"fill: red !important\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  Entries e;
  e.add("fill: green !important", 1);
  e.fix();
  GTEXT_CSS_Computed_Style * s = nullptr;
  ASSERT_EQ(gtext_css_style_compute(d.ctx, d.at("r"), nullptr, e.list.data(), e.list.size(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Cascade_Entry w;
  ASSERT_TRUE(gtext_css_computed_specified(s, find(R(), "fill"), &w));
  EXPECT_TRUE(w.element_attached);
  gtext_css_computed_free(s);
}

TEST(CssStyleEntries, TheDiagnosticsOfAnEntryNameItsIndex) {
  Doc d(svg("<rect id=\"r\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  Entries e;
  e.add("fill: red", 0);
  e.add("fill: nonsense; bogus: 1", 1);
  e.fix();
  e.list[1].base_offset = 500;
  GTEXT_CSS_Computed_Style * s = nullptr;
  ASSERT_EQ(gtext_css_style_compute(d.ctx, d.at("r"), nullptr, e.list.data(), e.list.size(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_computed_diagnostic_total(s), 2u);
  GTEXT_CSS_Style_Diagnostic g;
  ASSERT_TRUE(gtext_css_computed_diagnostic(s, 0, &g));
  EXPECT_EQ(g.source, GTEXT_CSS_STYLE_SOURCE_ENTRY);
  EXPECT_EQ(g.entry, 1u);
  EXPECT_EQ(g.offset, 500u);
  gtext_css_computed_free(s);
}

TEST(CssStyleEntries, AnEntryThatContradictsItsKeysIsRefused) {
  Doc d(svg("<rect id=\"r\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  Entries e;
  e.add("fill: red", 0);
  e.fix();
  e.list[0].keys.layer = 5; /* a layer, and the call has no layer registry */
  GTEXT_CSS_Computed_Style * s = nullptr;
  GTEXT_CSS_Compute_Result res;
  EXPECT_EQ(gtext_css_style_compute(d.ctx, d.at("r"), nullptr, e.list.data(), e.list.size(), nullptr, &s, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(s, nullptr);
  EXPECT_EQ(res.status, GTEXT_CSS_E_INVALID);
}

TEST(CssStyleEntries, TheCandidateLimitIsTheCallsLimit) {
  Doc d(svg("<rect id=\"r\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  GTEXT_CSS_Style_Options o = gtext_css_style_options_default();
  o.candidates.max_candidates = 2;
  Entries e;
  e.add("fill: red; stroke: blue; opacity: 0.5", 0);
  e.fix();
  GTEXT_CSS_Computed_Style * s = nullptr;
  GTEXT_CSS_Compute_Result res;
  EXPECT_EQ(gtext_css_style_compute(d.ctx, d.at("r"), nullptr, e.list.data(), e.list.size(), &o, &s, &res), GTEXT_CSS_E_LIMIT_CANDIDATES);
  EXPECT_EQ(s, nullptr);
  o.candidates.max_candidates = 3;
  EXPECT_EQ(gtext_css_style_compute(d.ctx, d.at("r"), nullptr, e.list.data(), e.list.size(), &o, &s, &res), GTEXT_CSS_OK) << "at the bound";
  gtext_css_computed_free(s);
}

/* ---- the presentation attributes ---------------------------------------------------------------------------------------- */

std::string text_of(const char * attribute, const std::string & value, const GTEXT_CSS_Presentation_Options * o = nullptr, size_t * err = nullptr) {
  const char * element = !strcmp(attribute, "d") ? "path" : !strcmp(attribute, "patternTransform") ? "pattern" : "rect";
  const GTEXT_CSS_Presentation_Row * row = gtext_css_presentation_find(nullptr, kSvg, strlen(kSvg), element, strlen(element), attribute, strlen(attribute));
  EXPECT_NE(row, nullptr) << attribute;
  if (!row) {
    return "<no row>";
  }
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_css_presentation_value_text(row, value.data(), value.size(), o, nullptr, &out, &n, err);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_allocator_free(nullptr, out);
  return s;
}

TEST(CssPresentation, TheSvgTransformSyntaxIsCssTransformFunctions) {
  EXPECT_EQ(text_of("transform", "translate(10,20)"), "translate(10px, 20px)");
  EXPECT_EQ(text_of("transform", "translate(10)"), "translate(10px)");
  EXPECT_EQ(text_of("transform", "scale(2) scale(2 3)"), "scale(2) scale(2, 3)");
  EXPECT_EQ(text_of("transform", "rotate(45)"), "rotate(45deg)");
  EXPECT_EQ(text_of("transform", "rotate(45 10 20)"), "translate(10px, 20px) rotate(45deg) translate(-10px, -20px)");
  EXPECT_EQ(text_of("transform", "rotate(45, -10, 0)"), "translate(-10px, 0px) rotate(45deg) translate(10px, 0px)");
  EXPECT_EQ(text_of("transform", "skewX(30)skewY(-5)"), "skewX(30deg) skewY(-5deg)");
  EXPECT_EQ(text_of("transform", "matrix(1 0 0 1 5 6)"), "matrix(1, 0, 0, 1, 5, 6)");
  EXPECT_EQ(text_of("transform", "matrix(1,0,0,1,5.,.5)"), "matrix(1, 0, 0, 1, 5.0, .5)") << "CSS does not read `5.`";
  EXPECT_EQ(text_of("transform", "translate(1e1-2)"), "translate(1e1px, -2px)") << "numbers need no separator before a sign";
  EXPECT_EQ(text_of("transform", "  "), "none");
  EXPECT_EQ(text_of("transform", " translate(1 2) , rotate(3) "), "translate(1px, 2px) rotate(3deg)");
  EXPECT_EQ(text_of("patternTransform", "scale(2)"), "scale(2)");
}

TEST(CssPresentation, AValueThatIsNotSvgSyntaxIsLeftForThePropertyToJudgeAndSaysWhereSvgFailed) {
  size_t err = 0;
  EXPECT_EQ(text_of("transform", "translate(10px)", nullptr, &err), "translate(10px)");
  EXPECT_EQ(err, 12u) << "at the `px` after the number";
  EXPECT_EQ(text_of("transform", "rotate(1 2)", nullptr, &err), "rotate(1 2)") << "two arguments are no rotate";
  EXPECT_EQ(err, 0u);
  EXPECT_EQ(text_of("transform", "translate(1,)", nullptr, &err), "translate(1,)");
  EXPECT_EQ(text_of("transform", "translate(1) ,", nullptr, &err), "translate(1) ,");
  EXPECT_EQ(text_of("transform", "bogus(1)", nullptr, &err), "bogus(1)");
  size_t ok = 7;
  EXPECT_EQ(text_of("transform", "scale(2)", nullptr, &ok), "scale(2)");
  EXPECT_EQ(ok, (size_t)-1);
  GTEXT_CSS_Presentation_Options o = gtext_css_presentation_options_default();
  o.svg_transform = false;
  EXPECT_EQ(text_of("transform", "translate(10)", &o), "translate(10)") << "without the option the value is the property's own";
}

TEST(CssPresentation, PathDataIsAString) {
  EXPECT_EQ(text_of("d", "M0 0 L10 10"), "\"M0 0 L10 10\"");
  EXPECT_EQ(text_of("d", " M0,0 \"x\\ "), "\"M0,0 \\\"x\\\\\"");
  EXPECT_EQ(text_of("d", "M0 0\nL1 1"), "\"M0 0\\a L1 1\"");
  EXPECT_EQ(text_of("d", ""), "none");
  GTEXT_CSS_Presentation_Options o = gtext_css_presentation_options_default();
  o.path_data = false;
  EXPECT_EQ(text_of("d", "path('M0 0')", &o), "path('M0 0')");
}

TEST(CssPresentation, TheTableSaysWhichAttributeIsOnWhichElement) {
  auto find_on = [](const char * ns, const char * element, const char * attribute) {
    return gtext_css_presentation_find(nullptr, ns, ns ? strlen(ns) : 0, element, strlen(element), attribute, strlen(attribute));
  };
  EXPECT_NE(find_on(kSvg, "circle", "cx"), nullptr);
  EXPECT_EQ(find_on(kSvg, "rect", "cx"), nullptr) << "cx is on circle and ellipse";
  EXPECT_NE(find_on(kSvg, "rect", "rx"), nullptr);
  EXPECT_EQ(find_on(kSvg, "circle", "rx"), nullptr);
  EXPECT_NE(find_on(kSvg, "path", "d"), nullptr);
  EXPECT_EQ(find_on(kSvg, "rect", "d"), nullptr);
  EXPECT_NE(find_on(kSvg, "anything", "fill"), nullptr);
  EXPECT_EQ(find_on(kSvg, "animate", "fill"), nullptr) << "an animation element's own fill attribute";
  EXPECT_NE(find_on(kSvg, "g", "transform"), nullptr);
  EXPECT_EQ(find_on(kSvg, "pattern", "transform"), nullptr);
  EXPECT_NE(find_on(kSvg, "pattern", "patternTransform"), nullptr);
  EXPECT_EQ(find_on(kSvg, "linearGradient", "transform"), nullptr);
  EXPECT_NE(find_on(kSvg, "radialGradient", "gradientTransform"), nullptr);
  EXPECT_EQ(find_on(kSvg, "rect", "patterntransform"), nullptr) << "attribute names are case-sensitive";
  EXPECT_EQ(find_on("http://www.w3.org/1999/xhtml", "rect", "fill"), nullptr) << "only in the SVG namespace";
  EXPECT_EQ(find_on(nullptr, "rect", "fill"), nullptr);
  EXPECT_EQ(find_on(kSvg, "rect", "no-such-attribute"), nullptr);
  const GTEXT_CSS_Presentation_Table * t = gtext_css_presentation_table_svg2();
  EXPECT_EQ(t->count, 72u);
  for (size_t i = 0; i < t->count; i++) {
    EXPECT_NE(find(R(), t->rows[i].property), 0u) << t->rows[i].property << " is a property of the registry";
  }
}

TEST(CssPresentation, ATableOfTheCallersIsUsedInstead) {
  static const GTEXT_CSS_Presentation_Row rows[] = {
      {"bgcolor", "background-color", false, "body,td", GTEXT_CSS_PRESENTATION_CSS},
      {"fill", "stroke", false, NULL, GTEXT_CSS_PRESENTATION_CSS},
  };
  GTEXT_CSS_Presentation_Table table = {rows, 2};
  GTEXT_CSS_Style_Options o = gtext_css_style_options_default();
  o.presentation.table = &table;
  Doc d(svg("<rect id=\"r\" fill=\"red\" bgcolor=\"blue\"/><td id=\"t\" bgcolor=\"green\" fill=\"red\"/>"), false, &o);
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.value("r", "fill"), "black") << "the table does not have SVG's fill";
  EXPECT_EQ(d.value("r", "stroke"), "red") << "it has its own";
  EXPECT_EQ(d.value("r", "background-color"), "transparent") << "bgcolor is on body and td only";
  EXPECT_EQ(d.value("t", "background-color"), "green");
}

TEST(CssPresentation, UnitlessLengthsAreUserUnitsInAnyLengthProperty) {
  Doc d(svg("<rect id=\"r\" stroke-width=\"2\" font-size=\"12\" letter-spacing=\"-1\" stroke-dashoffset=\"3\" stroke-dasharray=\"4,5 6\" x=\"7\" width=\"8\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("r", "stroke-width"), "2px");
  EXPECT_EQ(d.value("r", "font-size"), "12px");
  EXPECT_EQ(d.value("r", "letter-spacing"), "-1px");
  EXPECT_EQ(d.value("r", "stroke-dashoffset"), "3px");
  EXPECT_EQ(d.value("r", "stroke-dasharray"), "4px, 5px, 6px");
  EXPECT_EQ(d.value("r", "x"), "7px");
  EXPECT_EQ(d.value("r", "width"), "8px");
  GTEXT_CSS_Style_Options o = gtext_css_style_options_default();
  o.presentation.unitless_lengths = false;
  Doc e(svg("<rect id=\"r\" font-size=\"12\" stroke-width=\"2\"/>"), false, &o);
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  EXPECT_EQ(e.value("r", "font-size"), "16px") << "without the option a number is no length: dropped, and the initial value stands";
  EXPECT_EQ(gtext_css_computed_diagnostic_total(e.at("r")->style), 1u) << "font-size: 12 is not valid; stroke-width takes a number";
}

TEST(CssPresentation, TheStyleAttributeOfAnHtmlElementIsReadAndItsAttributesAreNotPresentation) {
  Doc d("<!doctype html><html><body><div id=\"x\" fill=\"red\" style=\"color: red\">t</div></body></html>", true);
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("x", "color"), "red");
  EXPECT_EQ(d.value("x", "fill"), "black") << "an HTML element has no presentation attribute `fill`";
  cmt::Inst * x = d.at("x");
  EXPECT_EQ(gtext_css_computed_diagnostic_total(x->style), 0u);
}

TEST(CssPresentation, AddingAnAttributeToACandidateSetMakesAHint) {
  cmp::Context ctx(R());
  GTEXT_CSS_Candidates * set = nullptr;
  ASSERT_EQ(gtext_css_candidates_new(R(), nullptr, nullptr, &set), GTEXT_CSS_OK);
  const GTEXT_CSS_Presentation_Row * row = gtext_css_presentation_find(nullptr, kSvg, strlen(kSvg), "rect", 4, "fill", 4);
  ASSERT_NE(row, nullptr);
  GTEXT_CSS_Cascade_Entry keys;
  memset(&keys, 0, sizeof keys);
  keys.important = true;           /* ignored */
  keys.element_attached = true;    /* ignored */
  keys.specificity.a = 5;          /* ignored */
  GTEXT_CSS_Presentation_Diagnostic pd;
  ASSERT_EQ(gtext_css_candidates_add_presentation(set, &keys, row, "red", 3, nullptr, &pd, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(pd.outcome, GTEXT_CSS_PRESENTATION_OK);
  ASSERT_EQ(gtext_css_candidates_add_presentation(set, &keys, row, "nonsense", 8, nullptr, &pd, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(pd.outcome, GTEXT_CSS_PRESENTATION_INVALID_VALUE);
  EXPECT_EQ(gtext_css_candidates_count(set), 1u);
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_candidates_entry(set, 0, &e));
  EXPECT_TRUE(e.presentational_hint);
  EXPECT_FALSE(e.important);
  EXPECT_FALSE(e.element_attached);
  EXPECT_EQ(e.specificity.a, 0u);
  EXPECT_EQ(e.origin, GTEXT_CSS_ORIGIN_AUTHOR);
  EXPECT_EQ(gtext_css_candidates_add_presentation(nullptr, &keys, row, "red", 3, nullptr, &pd, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_candidates_add_presentation(set, &keys, nullptr, "red", 3, nullptr, &pd, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_candidates_free(set);
  static const GTEXT_CSS_Presentation_Row unknown = {"x", "no-such-property", false, NULL, GTEXT_CSS_PRESENTATION_CSS};
  GTEXT_CSS_Candidates * set2 = nullptr;
  ASSERT_EQ(gtext_css_candidates_new(R(), nullptr, nullptr, &set2), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_candidates_add_presentation(set2, &keys, &unknown, "1", 1, nullptr, &pd, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(pd.outcome, GTEXT_CSS_PRESENTATION_UNKNOWN_PROPERTY);
  gtext_css_candidates_free(set2);
}

/* ---- one test for each layer the style call stands on (the planted defects of tools/oracle/CSS_STYLE_PLANTS are each seen to fail one of these) ---- */

TEST(CssStyleLayers, TheUnitsOfTheValuesEngineAreTheStandards) {
  /* Values 4, 6.2: 1in = 2.54cm = 25.4mm = 101.6Q = 72pt = 6pc = 96px */
  Doc d(svg("<rect id=\"r\" style=\"stroke-width: 3pt; letter-spacing: 1in; word-spacing: 6pc; text-indent: 2.54cm; margin-left: 25.4mm; margin-right: 101.6Q\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.value("r", "stroke-width"), "4px");
  EXPECT_EQ(d.value("r", "letter-spacing"), "96px");
  EXPECT_EQ(d.value("r", "word-spacing"), "96px");
  EXPECT_EQ(d.value("r", "text-indent"), "96px");
  EXPECT_EQ(d.value("r", "margin-left"), "96px");
  EXPECT_EQ(d.value("r", "margin-right"), "96px");
}

TEST(CssStyleLayers, ACalculationIsSimplified) {
  Doc d(svg("<rect id=\"r\" style=\"stroke-width: calc(2 * (1px + 2px)); letter-spacing: calc(1px + 2px - 0.5px); word-spacing: calc(10% + 5%); margin-top: calc(2 * (1px + 1em))\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.value("r", "stroke-width"), "6px");
  EXPECT_EQ(d.value("r", "letter-spacing"), "2.5px");
  EXPECT_EQ(d.value("r", "word-spacing"), "15%");
  EXPECT_EQ(d.value("r", "margin-top"), "34px") << "a number times a sum of unlike terms: 2px and 2em (32px)";
}

TEST(CssStyleLayers, AColorKeepsItsChannels) {
  Doc d(svg("<rect id=\"r\" stroke=\"#123456\" fill=\"rgb(10 20 30 / 0.5)\" color=\"#abc\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  const std::string stroke = d.value("r", "stroke");
  EXPECT_TRUE(stroke == "rgb(18, 52, 86)" || stroke == "#123456") << stroke;
  EXPECT_EQ(d.value("r", "fill"), "rgba(10, 20, 30, 0.5)");
  const std::string color = d.value("r", "color");
  EXPECT_TRUE(color == "rgb(170, 187, 204)" || color == "#abc" || color == "#aabbcc") << color;
  /* the channels are in the value, whatever the spelling: a color read back as another is another color */
  const GTEXT_CSS_Value * v = gtext_css_computed_root(d.at("r")->style, find(R(), "stroke"));
  ASSERT_NE(v, nullptr);
  ASSERT_EQ(v->kind, GTEXT_CSS_VAL_COLOR);
  ASSERT_NE(v->color, nullptr);
  ASSERT_GE(v->color->component_count, 3u);
  EXPECT_EQ(v->color->components[0].value, 18.0);
  EXPECT_EQ(v->color->components[1].value, 52.0);
  EXPECT_EQ(v->color->components[2].value, 86.0);
}

TEST(CssStyleLayers, InheritedPropertiesAreInheritedAndTheOthersAreNot) {
  Doc d(svg("<g id=\"g\" font-weight=\"bold\" font-style=\"italic\" opacity=\"0.5\" overflow-x=\"hidden\" fill=\"red\"><rect id=\"r\"/></g>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.value("r", "font-weight"), "700");
  EXPECT_EQ(d.value("r", "font-style"), "italic");
  EXPECT_EQ(d.value("r", "fill"), "red");
  EXPECT_EQ(d.value("r", "opacity"), "1") << "not inherited";
  EXPECT_EQ(d.value("r", "overflow-x"), "visible") << "not inherited";
}

TEST(CssStyleLayers, AShorthandIsDistributedAsTheStandardSays) {
  Doc d(svg("<rect id=\"r\" style=\"margin: 1px 2px 3px\"/><rect id=\"s\" style=\"padding: 1px 2px\"/><rect id=\"t\" style=\"margin: 1px 2px 3px 4px\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.value("r", "margin-top"), "1px");
  EXPECT_EQ(d.value("r", "margin-right"), "2px");
  EXPECT_EQ(d.value("r", "margin-bottom"), "3px");
  EXPECT_EQ(d.value("r", "margin-left"), "2px");
  EXPECT_EQ(d.value("s", "padding-top"), "1px");
  EXPECT_EQ(d.value("s", "padding-right"), "2px");
  EXPECT_EQ(d.value("s", "padding-bottom"), "1px");
  EXPECT_EQ(d.value("s", "padding-left"), "2px");
  EXPECT_EQ(d.value("t", "margin-left"), "4px");
}

TEST(CssStyleLayers, TheRuleOfAppearanceIsComparedBeforeTheDeclarationsOfTheList) {
  Doc d(svg("<rect id=\"r\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  Entries e;
  e.add("fill: green", 1);
  e.add("fill: blue", 0);
  e.fix();
  GTEXT_CSS_Computed_Style * s = nullptr;
  ASSERT_EQ(gtext_css_style_compute(d.ctx, d.at("r"), nullptr, e.list.data(), e.list.size(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  const GTEXT_CSS_Value * v = gtext_css_computed_root(s, find(R(), "fill"));
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_serialize_value_buffer(v, nullptr, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "green") << "rule 1 appears after rule 0, whichever the call was given first";
  gtext_css_serialize_buffer_free(nullptr, out);
  gtext_css_computed_free(s);
}

TEST(CssStyleLayers, TheBudgetOfSubstitutionStopsAValueThatDoubles) {
  GTEXT_CSS_Substitute_Options limits = gtext_css_substitute_options_default();
  limits.max_substituted_tokens = 400;
  limits.max_element_substituted_tokens = 3000;
  std::string style = "--n0: lol;";
  for (int i = 1; i <= 14; i++) {
    style += "--n" + std::to_string(i) + ": var(--n" + std::to_string(i - 1) + ") var(--n" + std::to_string(i - 1) + ");";
  }
  Doc d(svg("<rect id=\"r\" style=\"" + style + "\"/>"), false, nullptr, nullptr, &limits);
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  const GTEXT_CSS_Custom_Value * cv = nullptr;
  cmt::Inst * r = d.at("r");
  int highest = -1;
  for (int i = 0; i <= 14; i++) {
    const std::string name = "--n" + std::to_string(i);
    if (gtext_css_computed_custom_find(r->style, name.data(), name.size(), &cv)) {
      highest = i;
    }
  }
  EXPECT_EQ(highest, 7) << "255 tokens (128 words and the spaces between them) are under the budget of 400, 511 are over it";
  EXPECT_FALSE(gtext_css_computed_custom_find(r->style, "--n10", 5, &cv)) << "2,047 tokens are over it";
  EXPECT_FALSE(gtext_css_computed_custom_find(r->style, "--n14", 5, &cv));
}

TEST(CssStyleLayers, ThePresentationAttributeIsAHintNotARule) {
  /* a hint is under every rule whatever the order the rules and the attribute were met in */
  Doc d(svg("<rect id=\"r\" fill=\"red\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  Entries e;
  e.add("fill: green", 0);
  e.fix();
  e.list[0].keys.sheet = 0;
  GTEXT_CSS_Computed_Style * s = nullptr;
  ASSERT_EQ(gtext_css_style_compute(d.ctx, d.at("r"), nullptr, e.list.data(), e.list.size(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Cascade_Entry w;
  ASSERT_TRUE(gtext_css_computed_specified(s, find(R(), "fill"), &w));
  EXPECT_FALSE(w.presentational_hint);
  gtext_css_computed_free(s);
}


/* ---- the dump and the hash ---------------------------------------------------------------------------------------------- */

TEST(CssStyleLayers, DisplayContentsComputesToNoneOnAnSvgElementThatIsNoContainerOfRenderedContent) {
  /* Display 3, Appendix B: "display: contents computes to display: none." on the svg element and on the shapes; a, g, switch, use, tspan and textPath are
     stripped from the box tree instead, and text is not rendered. */
  Doc d(svg("<g id=\"g\" display=\"contents\"><rect id=\"r\" display=\"contents\"/></g><text id=\"t\" display=\"contents\"><tspan id=\"s\" display=\"contents\">x</tspan></text>"
            "<use id=\"u\" display=\"contents\" xlink:href=\"#r\"/>", "display=\"contents\""));
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("root", "display"), "none");
  EXPECT_EQ(d.value("g", "display"), "contents");
  EXPECT_EQ(d.value("r", "display"), "none");
  EXPECT_EQ(d.value("t", "display"), "none");
  EXPECT_EQ(d.value("s", "display"), "contents");
  EXPECT_EQ(d.value("u", "display"), "contents");
  Doc h("<div id=\"d\" style=\"display: contents\"><span id=\"s\" style=\"display: contents\"></span></div>", true);
  ASSERT_EQ(h.status, GTEXT_CSS_OK);
  EXPECT_EQ(h.value("d", "display"), "contents") << "an element of HTML is not an SVG element";
}

TEST(CssStyleLayers, TheUsedColorSchemeOfNormalIsTheUserAgentDefaultAndNotThePreference) {
  /* Color Adjust 1: "The element color scheme is the same as the page color scheme" and, with no support, "return the UA default color scheme". */
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.color_scheme = GTEXT_CSS_SCHEME_DARK;
  Doc d(svg("<rect id=\"n\" fill=\"light-dark(red, blue)\"/><rect id=\"ld\" fill=\"light-dark(red, blue)\" style=\"color-scheme: light dark\"/>"
            "<rect id=\"l\" fill=\"light-dark(red, blue)\" style=\"color-scheme: light\"/>"), false, nullptr, &env);
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("n", "fill"), "red") << "normal: the page declares no scheme";
  EXPECT_EQ(d.value("ld", "fill"), "blue") << "the element supports dark and the preference is dark";
  EXPECT_EQ(d.value("l", "fill"), "red");
}

TEST(CssStyleLayers, TheDashArrayIsOneCommaSeparatedListAndNegativeDashesAreInvalid) {
  /* SVG 2: the computed value of stroke-dasharray is "as comma separated list of absolute lengths or percentages"; Fill and Stroke 3: "Negative values are invalid." */
  Doc d(svg("<rect id=\"a\" style=\"stroke-dasharray: 1 2 3\"/><rect id=\"b\" style=\"stroke-dasharray: 1em, 2 3\"/><rect id=\"c\" style=\"stroke-dasharray: 4\"/>"
            "<rect id=\"n\" style=\"stroke-dasharray: 5 -1\"/><rect id=\"k\" style=\"stroke-dasharray: none\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("a", "stroke-dasharray"), "1px, 2px, 3px");
  EXPECT_EQ(d.value("b", "stroke-dasharray"), "16px, 2px, 3px");
  EXPECT_EQ(d.value("c", "stroke-dasharray"), "4px");
  EXPECT_EQ(d.value("n", "stroke-dasharray"), "none") << "dropped; the initial value stands";
  EXPECT_EQ(d.value("k", "stroke-dasharray"), "none");
}

TEST(CssStyleLayers, PaintOrderKeepsTheOrderItIsWrittenIn) {
  /* SVG 2: "If any of the three keywords are omitted, they are painted last, in the order they would be painted with paint-order: normal." */
  Doc d(svg("<rect id=\"a\" style=\"paint-order: markers fill\"/><rect id=\"b\" style=\"paint-order: stroke fill markers\"/><rect id=\"c\" style=\"paint-order: markers stroke\"/>"
            "<rect id=\"e\" style=\"paint-order: fill stroke markers\"/><rect id=\"f\" paint-order=\"stroke markers\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("a", "paint-order"), "markers fill") << "computed value: as specified";
  EXPECT_EQ(d.value("b", "paint-order"), "stroke fill markers");
  EXPECT_EQ(d.value("c", "paint-order"), "markers stroke");
  EXPECT_EQ(d.value("e", "paint-order"), "fill stroke markers");
  EXPECT_EQ(d.value("f", "paint-order"), "stroke markers");
}

TEST(CssStyleLayers, NormalWordSpacingComputesToZeroAndAZeroLetterSpacingToNormal) {
  /* Text 3 gives word-spacing the computed value "an absolute length", and normal is no additional spacing. */
  Doc d(svg("<rect id=\"a\"/><rect id=\"b\" style=\"word-spacing: 0\"/><rect id=\"c\" style=\"letter-spacing: normal\"/><rect id=\"z\" style=\"letter-spacing: 0\"/><rect id=\"e\" style=\"letter-spacing: 2px\"/>", "style=\"word-spacing: normal\""));
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name;
  EXPECT_EQ(d.value("root", "word-spacing"), "0px");
  EXPECT_EQ(d.value("a", "word-spacing"), "0px");
  EXPECT_EQ(d.value("c", "letter-spacing"), "normal");
  EXPECT_EQ(d.value("z", "letter-spacing"), "normal");
  EXPECT_EQ(d.value("e", "letter-spacing"), "2px");
}

TEST(CssStyleDump, ItIsVersionedSortedByTheBytesOfTheNamesAndOneLineAProperty) {
  Doc d(svg("<rect id=\"r\" style=\"--b: 1; --a: 2; -x-unknown: 1; fill: red\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  const std::string text = dump_of(d.at("r")->style);
  const std::string head = std::string("css-computed 1\nregistry ") + gtext_css_registry_pin_commit() + "\n";
  ASSERT_EQ(text.compare(0, head.size(), head), 0);
  EXPECT_EQ(GTEXT_CSS_COMPUTED_DUMP_VERSION, 1);
  std::vector<std::string> names;
  size_t pos = head.size();
  size_t lines = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    ASSERT_NE(nl, std::string::npos) << "every line ends with a line feed";
    const std::string line = text.substr(pos, nl - pos);
    const size_t colon = line.find(": ");
    ASSERT_NE(colon, std::string::npos) << line;
    names.push_back(line.substr(0, colon));
    pos = nl + 1;
    lines++;
  }
  for (size_t i = 1; i < names.size(); i++) {
    EXPECT_LT(names[i - 1], names[i]);
  }
  EXPECT_EQ(names[0], "--a");
  EXPECT_EQ(names[1], "--b");
  EXPECT_NE(text.find("\nfill: red\n"), std::string::npos);
  EXPECT_NE(text.find("\n--a: 2\n"), std::string::npos);
  size_t cursor = 0, props = 0;
  while (gtext_css_computed_next(d.at("r")->style, &cursor, nullptr, nullptr)) {
    props++;
  }
  EXPECT_EQ(lines, props + 2) << "one line for each property with a value, and the two custom properties";
}

TEST(CssStyleDump, TheHashIsTheSha256OfTheDump) {
  Doc d(svg("<rect id=\"r\" fill=\"red\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  const std::string text = dump_of(d.at("r")->style);
  unsigned char want[GSEC_SHA256_DIGEST_LEN], got[GTEXT_CSS_COMPUTED_HASH_LENGTH];
  ASSERT_EQ(gsec_sha256(text.data(), text.size(), want), GSEC_OK);
  ASSERT_EQ(gtext_css_computed_hash(d.at("r")->style, got), GTEXT_CSS_OK);
  EXPECT_EQ(hex(got, 32), hex(want, 32));
  EXPECT_EQ(gtext_css_computed_hash(d.at("r")->style, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_hash(nullptr, got), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_dump(nullptr, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  EXPECT_EQ(gtext_css_computed_dump(d.at("r")->style, &sink), GTEXT_CSS_E_INVALID) << "a sink with no write";
  sink.write = [](void *, const char *, size_t) -> GTEXT_Markup_Status { return GTEXT_MARKUP_E_OOM; };
  EXPECT_EQ(gtext_css_computed_dump(d.at("r")->style, &sink), GTEXT_CSS_E_WRITE);
}

TEST(CssStyleDump, ADumpHasNoLineBreakInsideAValue) {
  Doc d(svg("<rect id=\"r\" style=\"--a: 'x\\a y'; --b: a /* c\n d */ b; font-family: 'q\\a r'; content: 'z\\d w'\"/>"));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  const std::string text = dump_of(d.at("r")->style);
  size_t cursor = 0, props = 0;
  while (gtext_css_computed_next(d.at("r")->style, &cursor, nullptr, nullptr)) {
    props++;
  }
  size_t lines = 0;
  for (char c : text) {
    lines += c == '\n';
  }
  EXPECT_EQ(lines, props + 2 + 2) << text.substr(0, 200);
}

/* ---- the fixtures ------------------------------------------------------------------------------------------------------- */

/// The expected text of a fixture: for each element with an id, the hash of its full dump and the lines of the properties the fixture names.
std::string expected_of(Doc & d, const std::string & props_line) {
  std::vector<std::string> props;
  size_t i = 0;
  while (i < props_line.size()) {
    size_t j = props_line.find_first_of(" \n", i);
    if (j == std::string::npos) {
      j = props_line.size();
    }
    if (j > i) {
      props.push_back(props_line.substr(i, j - i));
    }
    i = j + 1;
  }
  std::string out;
  for (cmt::Inst * x : d.tree.order) {
    if (x->id.empty() && !x->clone) {
      continue;
    }
    out += "## " + x->path + "\n";
    unsigned char h[32];
    EXPECT_EQ(gtext_css_computed_hash(x->style, h), GTEXT_CSS_OK);
    out += "hash " + hex(h, 32) + "\n";
    const std::string text = dump_of(x->style);
    for (const std::string & p : props) {
      const std::string key = "\n" + p + ": ";
      const size_t at = text.find(key);
      if (at == std::string::npos) {
        out += p + ": <none>\n";
        continue;
      }
      const size_t end = text.find('\n', at + 1);
      out += text.substr(at + 1, end - at) ;
    }
  }
  return out;
}

class CssStyleFixtures : public testing::TestWithParam<const char *> {};

TEST_P(CssStyleFixtures, ComputeToTheirExpectedDumpsByteForByte) {
  const std::string name = GetParam();
  const std::string svg_path = find_file("data/css/svg/" + name + ".svg");
  ASSERT_NE(svg_path, "") << name;
  const std::string props_path = find_file("data/css/svg/" + name + ".props");
  ASSERT_NE(props_path, "");
  Doc d(slurp(svg_path));
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << d.result.name << " " << (d.result.detail ? d.result.detail : "");
  const std::string got = expected_of(d, slurp(props_path));
  const std::string expected_path = svg_path.substr(0, svg_path.size() - 4) + ".expected";
  if (getenv("GHOTI_REGENERATE_GOLDEN")) {
    std::ofstream(expected_path, std::ios::binary) << got;
  }
  const std::string want = slurp(expected_path);
  ASSERT_NE(want, "") << expected_path << " is missing: GHOTI_REGENERATE_GOLDEN=1 writes it";
  EXPECT_EQ(got, want) << name;
}

INSTANTIATE_TEST_SUITE_P(Svg, CssStyleFixtures, testing::Values("presentation-attributes", "inheritance", "use", "custom-properties"));

TEST(CssStyleFixturesSpot, TheValuesThatTheStandardGivesForTheFixtures) {
  Doc d(slurp(find_file("data/css/svg/presentation-attributes.svg")));
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.value("plain", "stroke-width"), "2px");
  EXPECT_EQ(d.value("plain", "fill"), "red");
  EXPECT_EQ(d.value("plain", "opacity"), "0.5");
  EXPECT_EQ(d.value("style-wins", "fill"), "green");
  EXPECT_EQ(d.value("style-wins", "stroke-width"), "5px");
  EXPECT_EQ(d.value("important", "fill"), "green");
  EXPECT_EQ(d.value("bad-attribute", "fill"), "black");
  EXPECT_EQ(d.value("bad-inherits", "fill"), "orange");
  EXPECT_EQ(d.value("explicit-inherit", "stroke"), "none");
  EXPECT_EQ(d.value("initial", "stroke-width"), "1px");
  EXPECT_EQ(d.value("text-units", "font-size"), "12px");
  Doc u(slurp(find_file("data/css/svg/use.svg")));
  ASSERT_EQ(u.status, GTEXT_CSS_OK);
  EXPECT_EQ(u.value("u1>shape>shape-own", "fill"), "red") << "inherit from the group that is the clone's parent in the use";
  EXPECT_EQ(u.value("u2>shape>shape-rect", "stroke"), "blue");
  EXPECT_EQ(u.value("u3>shape-rect", "stroke-width"), "7px") << "the clone inherits from the use";
}

} // namespace
