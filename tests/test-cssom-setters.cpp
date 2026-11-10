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
 * The attributes of a rule and their setters (selectorText, keyText, conditionText, the named attributes), and the `style` of the rules whose block the typed model reads (a page
 * rule, a keyframe, `@font-face`, `@counter-style`): what is read, what is ignored, that a rule keeps its identity, its place and the rules inside it when it is replaced as a whole,
 * that the version of the rule and of its sheet moves once and one event is sent, and that a refused edit changes nothing.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

std::string attr(GTEXT_CSSOM_Rule * r, const char * name, bool * is_null = nullptr) {
  char * out = nullptr;
  size_t n = 0;
  bool nul = false;
  const GTEXT_CSS_Status st = gtext_cssom_rule_get_attribute(r, name, std::strlen(name), &out, &n, &nul);
  if (is_null) {
    *is_null = nul;
  }
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r)->core->alloc, out);
  return s;
}

std::string selector_text(GTEXT_CSSOM_Rule * r) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_rule_selector_text(r, &out, &n);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r)->core->alloc, out);
  return s;
}

std::string key_text(GTEXT_CSSOM_Rule * r) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_rule_key_text(r, &out, &n);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r)->core->alloc, out);
  return s;
}

Result set_selector(GTEXT_CSSOM_Rule * r, const std::string & text) {
  Result res;
  res.st = gtext_cssom_rule_set_selector_text(r, text.data(), text.size(), &res.o);
  return res;
}

Result set_attr(GTEXT_CSSOM_Rule * r, const char * name, const std::string & value) {
  Result res;
  res.st = gtext_cssom_rule_set_attribute(r, name, std::strlen(name), value.data(), value.size(), &res.o);
  return res;
}

}  // namespace

TEST(CssomSetters, SelectorTextOfAStyleRuleIsReadAndReplacedAsAWhole) {
  Doc doc;
  Sheet s(doc, "a, b { color: red; & c { margin: 1px } } d {}");
  Rule r = path(s, {0});
  Rule inner = path(s, {0, 0});
  EXPECT_EQ(selector_text(r.r), "a, b");
  EXPECT_EQ(selector_text(inner.r), "& c");
  const uint64_t id = gtext_cssom_rule_id(r.r), version = gtext_cssom_rule_version(r.r), sheet_version = gtext_cssom_sheet_version(s.s);
  doc.events.clear();
  Result res = set_selector(r.r, "e > f,  g");
  ASSERT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(res.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(selector_text(r.r), "e > f, g");
  EXPECT_EQ(css(r.r), "e > f, g {\n  color: red;\n  & c { margin: 1px; }\n}") << "\"parse a group of selectors\": the declarations and the rule inside stay";
  EXPECT_EQ(gtext_cssom_rule_id(r.r), id) << "identity";
  EXPECT_EQ(gtext_cssom_rule_version(r.r), version + 1);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sheet_version + 1);
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(doc.events[0].kind, GTEXT_CSSOM_EVENT_RULE_REPLACED);
  EXPECT_EQ(doc.events[0].rule_id, id);
  Rule again = path(s, {0});
  EXPECT_EQ(again.r, r.r) << "the same object";
  Rule still = path(s, {0, 0});
  EXPECT_EQ(still.r, inner.r) << "the rule inside is the same object";
  GTEXT_CSSOM_Source_Info si;
  ASSERT_EQ(gtext_cssom_rule_source(r.r, &si), GTEXT_CSS_OK);
  EXPECT_EQ(si.kind, GTEXT_CSSOM_SOURCE_MODIFIED);
  EXPECT_EQ(shape(s), "style[style],style");
}

TEST(CssomSetters, ASelectorThatDoesNotParseIsIgnored) {
  Doc doc;
  Sheet s(doc, "a { color: red }");
  Rule r = path(s, {0});
  const std::string before = dump(s.s);
  doc.events.clear();
  for (const char * bad : {"", "a >", "1a", "a {} b", "a; b", ":bogus", "a,,b", "}"}) {
    Result res = set_selector(r.r, bad);
    EXPECT_EQ(res.st, GTEXT_CSS_OK) << bad;
    EXPECT_EQ(res.o.kind, GTEXT_CSSOM_IGNORED_INVALID_SELECTOR) << bad << " " << kind_name(res.o.kind);
    EXPECT_EQ(dump(s.s), before) << bad;
  }
  EXPECT_TRUE(doc.events.empty());
  EXPECT_EQ(gtext_cssom_rule_set_selector_text(nullptr, "a", 1, nullptr), GTEXT_CSS_E_INVALID);
  Sheet t(doc, "@media print { a {} }");
  Rule m = path(t, {0});
  EXPECT_EQ(gtext_cssom_rule_set_selector_text(m.r, "a", 1, nullptr), GTEXT_CSS_E_UNSUPPORTED) << "a media rule has no selectorText";
}

TEST(CssomSetters, ANestedRuleReadsItsSelectorRelativeToItsParent) {
  Doc doc;
  Sheet s(doc, "a { b {} }");
  Rule inner = path(s, {0, 0});
  EXPECT_EQ(set_selector(inner.r, "> c").o.kind, GTEXT_CSSOM_CHANGED) << "a relative selector is valid in a style rule";
  EXPECT_EQ(selector_text(inner.r), "& > c") << "written with its &, as a nested selector is";
  Sheet t(doc, "a {} b {}");
  Rule top = path(t, {1});
  EXPECT_EQ(set_selector(top.r, "> c").o.kind, GTEXT_CSSOM_IGNORED_INVALID_SELECTOR) << "and not at the top of a sheet";
  EXPECT_EQ(selector_text(top.r), "b");
}

TEST(CssomSetters, ARuleKeepsTheNamespacesItWasMadeIn) {
  Doc doc;
  Sheet s(doc, "@namespace x url(u); x|a {}");
  Rule r = path(s, {1});
  EXPECT_EQ(set_selector(r.r, "x|b, *|c").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(selector_text(r.r), "x|b, c") << "*| is no namespace restriction without a default namespace";
  EXPECT_EQ(set_selector(r.r, "y|b").o.kind, GTEXT_CSSOM_IGNORED_INVALID_SELECTOR) << "an undeclared prefix is not a selector";
}

TEST(CssomSetters, ADetachedRuleCanStillBeChanged) {
  Doc doc;
  Sheet s(doc, "a { color: red } b {}");
  Rule r = path(s, {0});
  ASSERT_EQ(delete_rule(s.s, 0), GTEXT_CSS_OK);
  const uint64_t sheet_version = gtext_cssom_sheet_version(s.s);
  doc.events.clear();
  EXPECT_EQ(set_selector(r.r, "z").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(r.r), "z { color: red; }");
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sheet_version) << "no sheet holds it any more";
  EXPECT_TRUE(doc.events.empty());
}

TEST(CssomSetters, TheBlockOfASetterRuleIsTheTruthAfterTheBlockWasMutated) {
  Doc doc;
  Sheet s(doc, "a { color: red }");
  Rule r = path(s, {0});
  Decl st = style_of(r.r);
  ASSERT_EQ(set(st.d, "margin", "1px").st, GTEXT_CSS_OK);
  ASSERT_EQ(set_selector(r.r, "b").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(r.r), "b { color: red; margin: 1px; }");
  EXPECT_EQ(get(st.d, "margin"), "1px");
}

TEST(CssomSetters, KeyTextIsNormalizedAndAnInvalidKeyIsASyntaxError) {
  Doc doc;
  Sheet s(doc, "@keyframes k { from { color: red } 50% { color: blue } }");
  Rule k = path(s, {0, 0});
  EXPECT_EQ(key_text(k.r), "0%");
  ASSERT_EQ(gtext_cssom_rule_set_key_text(k.r, "from, to", 8, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(key_text(k.r), "0%, 100%") << "from and to are 0% and 100%";
  EXPECT_EQ(css(path(s, {0}).r), "@keyframes k {\n  0%, 100% { color: red; }\n  50% { color: blue; }\n}");
  for (const char * bad : {"invalid", "", "150%", "-1%", "10% 20%", "10%,", "a"}) {
    const std::string before = dump(s.s);
    EXPECT_EQ(gtext_cssom_rule_set_key_text(k.r, bad, std::strlen(bad), nullptr), GTEXT_CSS_E_SYNTAX) << bad << ": \"throw a SyntaxError\"";
    EXPECT_EQ(dump(s.s), before) << bad;
  }
  EXPECT_STREQ(gtext_cssom_exception_name(GTEXT_CSS_E_SYNTAX), "SyntaxError");
  Sheet t(doc, "a {}");
  Rule a = path(t, {0});
  EXPECT_EQ(gtext_cssom_rule_set_key_text(a.r, "10%", 3, nullptr), GTEXT_CSS_E_UNSUPPORTED);
}

TEST(CssomSetters, ConditionTextIsReadOnly) {
  Doc doc;
  Sheet s(doc, "@media not all { a {} } @supports (display: grid) { b {} } @container card (min-width: 1px) { c {} }");
  Rule m = path(s, {0});
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_rule_condition_text(m.r, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "not all");
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(m.r)->core->alloc, out);
  const std::string before = dump(s.s);
  Result res = set_attr(m.r, "conditionText", "all");
  EXPECT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(res.o.kind, GTEXT_CSSOM_IGNORED_NOT_ALLOWED_HERE) << "CSS Conditional 3: readonly; WebIDL: assigning does nothing";
  EXPECT_EQ(dump(s.s), before);
  EXPECT_EQ(attr(path(s, {1}).r, "conditionText"), "(display: grid)");
  EXPECT_EQ(attr(path(s, {2}).r, "containerName"), "card");
  EXPECT_EQ(attr(path(s, {2}).r, "containerQuery"), "(min-width: 1px)");
}

TEST(CssomSetters, TheNamedAttributesOfTheOtherKinds) {
  Doc doc;
  Sheet s(doc, "@import url(a.css) layer(x) supports(display: grid) screen; @import 'b.css'; @namespace svg url(http://x/); @layer a, b.c; @layer z {} @scope (.a) to (.b) {} @scope (.a) {}"
               " @container (min-width: 1px) {} @counter-style c { suffix: '. ' } @keyframes \"q r\" {} @font-feature-values Font One {}");
  EXPECT_EQ(attr(path(s, {0}).r, "href"), "a.css");
  EXPECT_EQ(attr(path(s, {0}).r, "layerName"), "x");
  EXPECT_EQ(attr(path(s, {0}).r, "supportsText"), "display: grid");
  bool nul = false;
  EXPECT_EQ(attr(path(s, {1}).r, "href"), "b.css");
  EXPECT_EQ(attr(path(s, {1}).r, "layerName", &nul), "");
  EXPECT_TRUE(nul) << "layerName is null without a layer";
  EXPECT_EQ(attr(path(s, {2}).r, "namespaceURI"), "http://x/");
  EXPECT_EQ(attr(path(s, {2}).r, "prefix"), "svg");
  EXPECT_EQ(attr(path(s, {3}).r, "nameList"), "a, b.c");
  EXPECT_EQ(attr(path(s, {4}).r, "name"), "z");
  EXPECT_EQ(attr(path(s, {5}).r, "start"), ".a");
  EXPECT_EQ(attr(path(s, {5}).r, "end"), ".b");
  EXPECT_EQ(attr(path(s, {6}).r, "end", &nul), "");
  EXPECT_TRUE(nul);
  EXPECT_EQ(attr(path(s, {7}).r, "containerName"), "");
  EXPECT_EQ(attr(path(s, {8}).r, "name"), "c");
  EXPECT_EQ(attr(path(s, {8}).r, "suffix"), ". ");
  EXPECT_EQ(attr(path(s, {9}).r, "name"), "q r") << "the name is decoded";
  EXPECT_EQ(attr(path(s, {10}).r, "fontFamily"), "Font One");
  EXPECT_EQ(attr(path(s, {0}).r, "bogus"), "<not supported>");
  bool writable = true;
  EXPECT_TRUE(gtext_cssom_rule_has_attribute(path(s, {0}).r, "href", 4, &writable));
  EXPECT_FALSE(writable);
  EXPECT_TRUE(gtext_cssom_rule_has_attribute(path(s, {8}).r, "symbols", 7, &writable));
  EXPECT_TRUE(writable);
  EXPECT_FALSE(gtext_cssom_rule_has_attribute(path(s, {8}).r, "href", 4, nullptr));
  Sheet p(doc, "@property --x { syntax: '<length>'; inherits: false; initial-value: 0px } @property --y { syntax: '*'; inherits: true }");
  EXPECT_EQ(attr(path(p, {0}).r, "name"), "--x");
  EXPECT_EQ(attr(path(p, {0}).r, "syntax"), "<length>");
  EXPECT_EQ(attr(path(p, {0}).r, "inherits"), "false");
  EXPECT_EQ(attr(path(p, {0}).r, "initialValue"), "0px");
  EXPECT_EQ(attr(path(p, {1}).r, "initialValue", &nul), "");
  EXPECT_TRUE(nul);
}

// CSSPropertyRule: `name`, `syntax` ("exactly as specified"), `inherits` and `initialValue` (null when absent). The descriptors are optional, so a rule that omits one has its initial value:
// the syntax `*` and `inherits` true; a descriptor that is ignored is as omitted; a statement has the syntax and initial value it was written with; of a prelude that lists names the name is
// empty (the CSSWG's resolution of w3c/csswg-drafts#14227, 2026-08-06) and `names` has them all.
TEST(CssomSetters, ThePropertyRuleHasTheDefaultsOfItsOmittedDescriptors) {
  Doc doc;
  Sheet p(doc, "@property --a {} @property --b { syntax: '<nosuch>'; inherits: maybe; initial-value: 5 } @property --c { syntax: '<length>'; initial-value: red } @property --d <length>: 4px;"
               " @property --e; @property --f type(<length> | auto); @property --g, --h { inherits: false }");
  bool nul = false;
  EXPECT_EQ(attr(path(p, {0}).r, "name"), "--a");
  EXPECT_EQ(attr(path(p, {0}).r, "syntax"), "*");
  EXPECT_EQ(attr(path(p, {0}).r, "inherits"), "true");
  EXPECT_EQ(attr(path(p, {0}).r, "initialValue", &nul), "");
  EXPECT_TRUE(nul);
  EXPECT_EQ(attr(path(p, {1}).r, "syntax"), "*");
  EXPECT_EQ(attr(path(p, {1}).r, "inherits"), "true");
  EXPECT_EQ(attr(path(p, {1}).r, "initialValue"), "5");
  EXPECT_EQ(attr(path(p, {2}).r, "syntax"), "<length>");
  EXPECT_EQ(attr(path(p, {2}).r, "initialValue", &nul), "");
  EXPECT_TRUE(nul) << "ignored: it is no <length>";
  EXPECT_EQ(attr(path(p, {3}).r, "name"), "--d");
  EXPECT_EQ(attr(path(p, {3}).r, "syntax"), "<length>");
  EXPECT_EQ(attr(path(p, {3}).r, "inherits"), "true");
  EXPECT_EQ(attr(path(p, {3}).r, "initialValue"), "4px");
  EXPECT_EQ(attr(path(p, {4}).r, "name"), "--e");
  EXPECT_EQ(attr(path(p, {4}).r, "syntax"), "*");
  EXPECT_EQ(attr(path(p, {5}).r, "syntax"), "<length> | auto");
  EXPECT_EQ(attr(path(p, {6}).r, "name"), "") << "\"return empty string for the `name` in that case\"";
  EXPECT_EQ(attr(path(p, {6}).r, "inherits"), "false");
}

namespace {
// CSSPropertyRule.names as the strings of the list, or "<status>" when the call fails.
std::vector<std::string> names_of(GTEXT_CSSOM_Rule * r, GTEXT_CSS_Status * status = nullptr) {
  GTEXT_CSSOM_String_List l{};
  const GTEXT_CSS_Status st = gtext_cssom_property_get_names(r, &l);
  if (status) {
    *status = st;
  }
  std::vector<std::string> v;
  if (st != GTEXT_CSS_OK) {
    return v;
  }
  for (size_t i = 0; i < l.count; i++) {
    v.emplace_back(l.items[i].text, l.items[i].length);
  }
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r)->core->alloc, static_cast<char *>(l.block));
  return v;
}
}  // namespace

// The CSSWG resolved on 2026-08-06 (w3c/csswg-drafts#14227, "Needs Edits"): "if single @property has multiple names, it will be represented in CSSOM as a single object with a `names` array
// and not expand into multiple objects and return empty string for the `name` in that case". The pinned draft's Issue box still says the question is unresolved; this follows the resolution.
TEST(CssomSetters, APropertyRuleOfSeveralNamesIsOneObjectWithANamesArrayAndAnEmptyName) {
  Doc doc;
  Sheet p(doc, "@property --a, --b, --c { syntax: '<length>'; inherits: false; initial-value: 0px } @property --d { inherits: true } @property --e <length>: 1px; @property --f, --f {}");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_list_length(p.rules()), 4u) << "not expanded into one object per name";
  EXPECT_EQ(attr(path(p, {0}).r, "name"), "");
  EXPECT_EQ(names_of(path(p, {0}).r), (std::vector<std::string>{"--a", "--b", "--c"}));
  EXPECT_EQ(attr(path(p, {0}).r, "syntax"), "<length>") << "the descriptors are the rule's";
  EXPECT_EQ(attr(path(p, {1}).r, "name"), "--d");
  EXPECT_EQ(names_of(path(p, {1}).r), (std::vector<std::string>{"--d"})) << "a rule of one name has an array of one";
  EXPECT_EQ(names_of(path(p, {2}).r), (std::vector<std::string>{"--e"})) << "the statement defines one name";
  EXPECT_EQ(attr(path(p, {2}).r, "name"), "--e");
  EXPECT_EQ(attr(path(p, {3}).r, "name"), "") << "two names, even when they are the same name";
  EXPECT_EQ(names_of(path(p, {3}).r), (std::vector<std::string>{"--f", "--f"}));
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  Doc d2;
  Sheet t(d2, "a { color: red } @media print {}");
  names_of(at(t.rules(), 0).r, &st);
  EXPECT_EQ(st, GTEXT_CSS_E_UNSUPPORTED) << "only a CSSPropertyRule has names";
  names_of(at(t.rules(), 1).r, &st);
  EXPECT_EQ(st, GTEXT_CSS_E_UNSUPPORTED);
  GTEXT_CSSOM_String_List none{};
  EXPECT_EQ(gtext_cssom_property_get_names(nullptr, &none), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_property_get_names(at(t.rules(), 0).r, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_FALSE(gtext_cssom_rule_has_attribute(path(p, {0}).r, "names", 5, nullptr)) << "a list is read through gtext_cssom_property_get_names(), not as a string";
}

// A text round trip keeps every name, so the document's reading of a changed rule (it re-reads the text) loses none.
TEST(CssomSetters, TheNamesOfAPropertyRuleSurviveItsText) {
  Doc doc;
  Sheet p(doc, "@property --a,--b { syntax: '*' }");
  const std::string text = css(at(p.rules(), 0).r);
  EXPECT_EQ(text, "@property --a, --b { syntax: \"*\"; inherits: true; }");
  Doc d2;
  Sheet q(d2, text.c_str());
  EXPECT_EQ(names_of(at(q.rules(), 0).r), (std::vector<std::string>{"--a", "--b"}));
  EXPECT_EQ(css(at(q.rules(), 0).r), text);
}

TEST(CssomSettersOom, EveryAllocationOfReadingTheNamesFailsCleanly) {
  const char * const text = "@property --alpha, --beta, --gamma { syntax: '<length>'; initial-value: 1px }";
  for (size_t n = 1; n < 1000; n++) {
    FaultAllocator fa;
    bool done = false;
    {
      Doc doc(nullptr, &fa.alloc);
      Sheet s(doc, text);
      ASSERT_EQ(s.status, GTEXT_CSS_OK);
      Rule r = at(s.rules(), 0);
      fa.arm(n);
      GTEXT_CSS_Status st = GTEXT_CSS_OK;
      const auto v = names_of(r.r, &st);
      done = !fa.failed;
      fa.disarm();
      if (st == GTEXT_CSS_OK) {
        EXPECT_EQ(v, (std::vector<std::string>{"--alpha", "--beta", "--gamma"})) << n;
      }
      else {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      EXPECT_EQ(names_of(r.r), (std::vector<std::string>{"--alpha", "--beta", "--gamma"})) << "with memory again, after " << n;
    }
    EXPECT_TRUE(fa.live.empty()) << "allocation " << n << " leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
    if (done) {
      break;
    }
  }
}

TEST(CssomSetters, ACounterStyleDescriptorIsReplacedAsAWhole) {
  Doc doc;
  Sheet s(doc, "@counter-style c { system: cyclic; symbols: a b; suffix: '. ' }");
  Rule c = path(s, {0});
  doc.events.clear();
  Result res = set_attr(c.r, "symbols", "x y z");
  ASSERT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(res.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(c.r), "@counter-style c { system: cyclic; symbols: x y z; suffix: \". \"; }");
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(doc.events[0].kind, GTEXT_CSSOM_EVENT_RULE_REPLACED);
  const std::string before = dump(s.s);
  EXPECT_EQ(set_attr(c.r, "system", "invalid (").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(dump(s.s), before) << "an invalid value changes nothing";
  EXPECT_EQ(set_attr(c.r, "suffix", "").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << "CSS Counter Styles 3, 9.2: an invalid value (the empty text is one) does nothing";
  EXPECT_EQ(css(c.r), "@counter-style c { system: cyclic; symbols: x y z; suffix: \". \"; }");
  EXPECT_EQ(set_attr(c.r, "pad", "3 '0'").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(attr(c.r, "pad"), "3 \"0\"");
  // found by the hunt of the differential (doc 5561): the name is parsed as a <counter-style-name>, so two identifiers are not one name; both browsers do nothing
  EXPECT_EQ(set_attr(c.r, "name", "foo bar").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(attr(c.r, "name"), "c");
  EXPECT_EQ(set_attr(c.r, "name", " foo-bar ").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(attr(c.r, "name"), "foo-bar");
  EXPECT_EQ(set_attr(c.r, "name", "9a").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(set_attr(c.r, "name", "none").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << "none is not a counter style name";
  EXPECT_EQ(set_attr(c.r, "href", "x").st, GTEXT_CSS_E_UNSUPPORTED);
}

TEST(CssomSetters, AKeyframesNameIsWrittenAsAnIdentifierOrAString) {
  Doc doc;
  Sheet s(doc, "@keyframes k { from { color: red } }");
  Rule k = path(s, {0});
  Rule child = path(s, {0, 0});
  ASSERT_EQ(set_attr(k.r, "name", "x y").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(attr(k.r, "name"), "x y");
  EXPECT_EQ(path(s, {0, 0}).r, child.r) << "the keyframes inside are the same objects";
  EXPECT_EQ(css(k.r), "@keyframes x\\ y {\n  0% { color: red; }\n}");
  ASSERT_EQ(set_attr(k.r, "name", "none").o.kind, GTEXT_CSSOM_CHANGED) << "a keyframes name may be a string";
  EXPECT_EQ(attr(k.r, "name"), "none");
}

/* ---- the block of a typed rule ----------------------------------------------------------------------------------------------------------------- */

TEST(CssomSetters, ThePageBlockExpandsMarginsAndKeepsDescriptors) {
  Doc doc;
  Sheet s(doc, "@page { size: a3; margin: 1em 24px 2in 101.5mm }");
  Rule p = path(s, {0});
  Decl st = style_of(p.r);
  ASSERT_TRUE(st.d);
  EXPECT_EQ(gtext_cssom_decl_owner(st.d), GTEXT_CSSOM_DECL_RULE);
  EXPECT_EQ(length(st.d), 5u) << "four margins and the size descriptor";
  EXPECT_EQ(get(st.d, "margin-top"), "1em");
  EXPECT_EQ(get(st.d, "margin-left"), "101.5mm");
  EXPECT_EQ(get(st.d, "margin"), "1em 24px 2in 101.5mm");
  EXPECT_EQ(get(st.d, "size"), "a3");
  EXPECT_EQ(get(st.d, "SIZE"), "a3") << "descriptor names are ASCII case-insensitive";
  EXPECT_EQ(item(st.d, 4), "size") << "the descriptors follow the properties";
  EXPECT_EQ(text_of(st.d), "margin: 1em 24px 2in 101.5mm; size: a3;");
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_decl_idl_get(st.d, "marginTop", 9, &out, &n), GTEXT_CSS_OK);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(p.r)->core->alloc, out);
  ASSERT_EQ(gtext_cssom_decl_idl_get(st.d, "pageOrientation", 15, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(n, 0u);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(p.r)->core->alloc, out);
}

TEST(CssomSetters, APageBlockIsEditedThroughTheRule) {
  Doc doc;
  Sheet s(doc, "@page { size: a3 }");
  Rule p = path(s, {0});
  Decl st = style_of(p.r);
  const uint64_t rv = gtext_cssom_rule_version(p.r), sv = gtext_cssom_sheet_version(s.s);
  doc.events.clear();
  Result r = set(st.d, "size", "portrait");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(st.d, "size"), "portrait");
  EXPECT_EQ(css(p.r), "@page { size: portrait; }") << "the rule says what the block says";
  EXPECT_EQ(gtext_cssom_rule_version(p.r), rv + 1);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sv + 1);
  EXPECT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(set(st.d, "size", "notarealsize").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(get(st.d, "size"), "portrait");
  EXPECT_EQ(set(st.d, "size", "portrait").o.kind, GTEXT_CSSOM_UNCHANGED) << "what the rule already says is not a change";
  EXPECT_EQ(gtext_cssom_rule_version(p.r), rv + 1);
  EXPECT_EQ(set(st.d, "page-orientation", "rotate-left").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(set(st.d, "page-orientation", "schmotate").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(get(st.d, "page-orientation"), "rotate-left");
  EXPECT_EQ(set(st.d, "margin-left", "50%").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(length(st.d), 3u);
  EXPECT_EQ(set(st.d, "margin", "auto").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(length(st.d), 6u) << "four margins and two descriptors";
  EXPECT_EQ(get(st.d, "margin-left"), "auto");
  Result rm = remove(st.d, "margin");
  EXPECT_EQ(rm.old, "auto");
  EXPECT_EQ(length(st.d), 2u);
  Result rd = remove(st.d, "size");
  EXPECT_EQ(rd.old, "portrait");
  EXPECT_EQ(get(st.d, "size"), "");
  EXPECT_EQ(remove(st.d, "size").old, "") << "removing what is not there is a no-op";
  EXPECT_EQ(set_text(st.d, "size: jis-b5 landscape; margin-top: 1px; bogus: 1").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(st.d), "margin-top: 1px; size: jis-b5 landscape;");
  EXPECT_EQ(css(p.r), "@page { size: jis-b5 landscape; margin-top: 1px; }") << "the rule keeps the order of the text it was set from";
  EXPECT_EQ(set_text(st.d, "margin-top: 1px; size: jis-b5 landscape;").o.kind, GTEXT_CSSOM_UNCHANGED);
}

TEST(CssomSetters, APageTakesPagePropertiesOnly) {
  Doc doc;
  Sheet s(doc, "@page { margin-top: 10px; transform: scale(1); float: left; color: red; --x: 1 }");
  Rule p = path(s, {0});
  Decl st = style_of(p.r);
  EXPECT_EQ(length(st.d), 3u) << "\"The @page rule can only contain page properties\" (Appendix A): margin-top, color and a custom property";
  EXPECT_EQ(get(st.d, "transform"), "");
  EXPECT_EQ(set(st.d, "transform", "scale(1)").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(set(st.d, "float", "left").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(set_text(st.d, "margin-bottom: 10px; transform: scale(1);").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(length(st.d), 1u);
  EXPECT_TRUE(gtext_cssom_decl_has_attribute(st.d, "marginTop", 9));
  EXPECT_TRUE(gtext_cssom_decl_has_attribute(st.d, "pageOrientation", 15));
  EXPECT_TRUE(gtext_cssom_decl_has_attribute(st.d, "size", 4));
  EXPECT_FALSE(gtext_cssom_decl_has_attribute(st.d, "cssFloat", 8)) << "a page's block has no cssFloat";
  EXPECT_FALSE(gtext_cssom_decl_has_attribute(st.d, "transform", 9));
  Sheet t(doc, "a { color: red }");
  Rule a = path(t, {0});
  Decl sd = style_of(a.r);
  EXPECT_TRUE(gtext_cssom_decl_has_attribute(sd.d, "cssFloat", 8));
  EXPECT_FALSE(gtext_cssom_decl_has_attribute(sd.d, "pageOrientation", 15));
}

TEST(CssomSetters, AKeyframeBlockRefusesWhatAKeyframeDoesNotTake) {
  Doc doc;
  Sheet s(doc, "@keyframes k { from { margin-top: 10px; animation-name: none } }");
  Rule k = path(s, {0, 0});
  Decl st = style_of(k.r);
  ASSERT_TRUE(st.d);
  EXPECT_EQ(length(st.d), 1u) << "animation-name does not apply to keyframes";
  EXPECT_EQ(get(st.d, "margin-top"), "10px");
  EXPECT_EQ(set(st.d, "animation-name", "none").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(length(st.d), 1u);
  EXPECT_EQ(set_text(st.d, "margin-bottom: 10px; animation-name: none;").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(length(st.d), 1u);
  EXPECT_EQ(get(st.d, "margin-bottom"), "10px");
  EXPECT_EQ(set(st.d, "color", "red", "important").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << "an !important declaration in a keyframe is ignored";
  EXPECT_EQ(set(st.d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(path(s, {0}).r), "@keyframes k {\n  0% { margin-bottom: 10px; color: red; }\n}");
}

TEST(CssomSetters, AFontFaceBlockHoldsDescriptors) {
  Doc doc;
  Sheet s(doc, "@font-face { font-family: A; src: url(a.woff2) format(\"woff2\"); bogus: 1 }");
  Rule f = path(s, {0});
  Decl st = style_of(f.r);
  EXPECT_EQ(length(st.d), 2u);
  EXPECT_EQ(get(st.d, "font-family"), "A");
  EXPECT_EQ(get(st.d, "src"), "url(\"a.woff2\") format(\"woff2\")");
  EXPECT_EQ(set(st.d, "font-display", "swap").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(set(st.d, "src", "bogus bogus").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(set(st.d, "color", "red").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << "a property is not a descriptor of @font-face";
  EXPECT_EQ(set(st.d, "font-display", "swap", "important").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << "a descriptor is not !important";
  EXPECT_EQ(priority(st.d, "font-display"), "");
  EXPECT_EQ(set(st.d, "font-display", "swap", "bogus").o.kind, GTEXT_CSSOM_IGNORED_BAD_PRIORITY);
  EXPECT_EQ(css(f.r), "@font-face { font-family: A; src: url(\"a.woff2\") format(\"woff2\"); font-display: swap; }");
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_decl_idl_get(st.d, "fontDisplay", 11, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "swap");
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(f.r)->core->alloc, out);
  EXPECT_EQ(gtext_cssom_decl_idl_set(st.d, "fontDisplay", 11, "block", 5, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(get(st.d, "font-display"), "block");
  GTEXT_CSS_Status status;
  Sheet m(doc, "@media print { a {} }");
  Rule mr = path(m, {0});
  style_of(mr.r, &status);
  EXPECT_EQ(status, GTEXT_CSS_E_UNSUPPORTED) << "a grouping rule has no style";
}

TEST(CssomSetters, TheBlockIsTheSameObjectAndFollowsTheRule) {
  Doc doc;
  Sheet s(doc, "@counter-style c { system: cyclic; symbols: a b }");
  Rule c = path(s, {0});
  Decl st = style_of(c.r);
  Decl again = style_of(c.r);
  EXPECT_EQ(st.d, again.d);
  ASSERT_EQ(set_attr(c.r, "symbols", "x y").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(st.d, "symbols"), "x y") << "the block is read again after the rule was replaced";
  EXPECT_EQ(set(st.d, "suffix", "'! '").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(attr(c.r, "suffix"), "! ");
}

TEST(CssomSetters, ADetachedTypedBlockStillWorks) {
  Doc doc;
  Sheet s(doc, "@keyframes k { 50% { color: red } }");
  Rule k = path(s, {0, 0});
  Rule parent = path(s, {0});
  Decl st = style_of(k.r);
  ASSERT_EQ(gtext_cssom_keyframes_delete_rule(parent.r, "50%", 3, nullptr), GTEXT_CSS_OK);
  const uint64_t sv = gtext_cssom_sheet_version(s.s);
  EXPECT_EQ(set(st.d, "margin-top", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(k.r), "50% { color: red; margin-top: 1px; }");
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sv);
  EXPECT_EQ(gtext_cssom_rule_set_key_text(k.r, "10%", 3, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(key_text(k.r), "10%");
}

TEST(CssomSetters, ASetterIsRefusedFromInsideACallback) {
  Doc doc;
  Sheet s(doc, "a {} @counter-style c { symbols: a b }");
  Rule r = path(s, {0});
  Rule c = path(s, {1});
  Result inner, inner_attr;
  doc.inside = [&]() {
    inner = set_selector(r.r, "b");
    inner_attr = set_attr(c.r, "symbols", "x");
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(delete_rule(s.s, 1), GTEXT_CSS_OK);
  EXPECT_EQ(inner.st, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(inner_attr.st, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(selector_text(r.r), "a");
}

TEST(CssomSetters, TheDescriptorsOfAColorProfileAreItsAttributes) {
  // css-color-5.idl (CSSColorProfileRule): name, src, renderingIntent, components, all readonly.
  Doc doc;
  Sheet s(doc, "@color-profile --swop5c { src: url(\"swop5c.icc\"); rendering-intent: perceptual; components: cyan, magenta, yellow, black }");
  Rule r = path(s, {0});
  EXPECT_EQ(attr(r.r, "name"), "--swop5c");
  EXPECT_EQ(attr(r.r, "src"), "url(\"swop5c.icc\")");
  EXPECT_EQ(attr(r.r, "renderingIntent"), "perceptual");
  EXPECT_EQ(attr(r.r, "components"), "cyan, magenta, yellow, black");
  bool writable = true;
  EXPECT_TRUE(gtext_cssom_rule_has_attribute(r.r, "renderingIntent", 15, &writable));
  EXPECT_FALSE(writable);
}

TEST(CssomSetters, ATypedBlocksCssTextIsWhatTheDeclarationBlockSaysOfIt) {
  // the browsers agree (the differential, css_cssom_gen.py): `style.cssText = "margin: 1px; margin-top: 2px"` of an @page or a keyframe is `margin: 2px 1px 1px`, and a later
  // declaration does not replace an important one; CSSOM, "update a CSS declaration block": the declarations are set one by one
  Doc doc;
  Sheet s(doc, "@page { margin: 1px } @keyframes k { from { opacity: 0 } }");
  Rule page = path(s, {0});
  Decl d = style_of(page.r);
  ASSERT_NE(d.d, nullptr);
  ASSERT_EQ(set_text(d.d, "margin: 1px; margin-top: 2px").st, GTEXT_CSS_OK);
  EXPECT_EQ(css(page.r), "@page { margin: 2px 1px 1px; }");
  EXPECT_EQ(length(d.d), 4u);
  ASSERT_EQ(set_text(d.d, "color: red !important; color: blue").st, GTEXT_CSS_OK);
  EXPECT_EQ(css(page.r), "@page { color: red !important; }");
  Rule frame = path(s, {1, 0});
  Decl f = style_of(frame.r);
  ASSERT_NE(f.d, nullptr);
  ASSERT_EQ(set_text(f.d, "margin: 1px; margin-top: 2px; margin-left: 3px").st, GTEXT_CSS_OK);
  EXPECT_EQ(css(frame.r), "0% { margin: 2px 1px 1px 3px; }");
}

TEST(CssomSetters, APageAndItsMarginBoxesTakeTheListsOfAppendixA) {
  // Paged Media 3, 4.3: "The @page rule can only contain page properties and margin at-rules. The margin at-rules can only contain page-margin properties." and Appendix A, which lists
  // the properties of each context one by one (the prefixes of the categories it names are not the list)
  Doc doc;
  Sheet s(doc, "@page { direction: rtl; unicode-bidi: embed; background-clip: content-box; border-radius: 1px; font-kerning: none; display: block; min-height: 1px; --x: 1; text-shadow: none;"
               " @top-left { unicode-bidi: embed; content: \"x\"; z-index: 1; background-clip: content-box; display: block; --y: 2 } }");
  // custom properties: in no category of the appendix; kept in the page context (Chromium drops and Firefox keeps: the draft is silent, the browsers differ), dropped in a margin box
  // (Chromium drops; Firefox 158 has no margin boxes to ask)
  EXPECT_EQ(css(path(s, {0}).r),
      "@page {\n  direction: rtl; min-height: 1px; --x: 1;\n  @top-left { unicode-bidi: embed; content: \"x\"; z-index: 1; }\n}");
}

TEST(CssomSetters, APseudoPageIsAKeywordAndIsWrittenInLowerCase) {
  // the pinned css/cssom/cssom-pagerule.html: "named:First" is "named:first"; the page name keeps its case
  Doc doc;
  Sheet s(doc, "@page Named:First:LEFT { }");
  Rule r = path(s, {0});
  EXPECT_EQ(selector_text(r.r), "Named:first:left");
  ASSERT_EQ(set_selector(r.r, "other:First").st, GTEXT_CSS_OK);
  EXPECT_EQ(selector_text(r.r), "other:first");
  EXPECT_EQ(css(r.r), "@page other:first { }");
}
