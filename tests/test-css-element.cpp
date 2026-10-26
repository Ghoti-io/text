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
 * The element interface and the environment (css_element.h), each test named for the rule it holds: a table smaller than the header's, a
 * callback that is needed and absent (GTEXT_CSS_E_UNSUPPORTED naming it), the inheritance parent that defaults to the parent, and the environment's
 * defaults and checks.
 */

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

namespace {

struct Tree {
  // elements are indices: 0 is none, 1..n are elements
  std::vector<int> parent, inh_parent;
  std::vector<std::string> names, ids;
  std::vector<std::vector<std::string>> classes;
  int calls = 0;
};

Tree * T(void * u) {
  return static_cast<Tree *>(u);
}

int idx(GTEXT_CSS_Element_Handle h) {
  return static_cast<int>(reinterpret_cast<intptr_t>(h));
}

GTEXT_CSS_Element_Handle h(int i) {
  return reinterpret_cast<GTEXT_CSS_Element_Handle>(static_cast<intptr_t>(i));
}

GTEXT_CSS_Element_Interface full_table(Tree & t) {
  GTEXT_CSS_Element_Interface i;
  std::memset(&i, 0, sizeof i);
  i.size = sizeof i;
  i.user = &t;
  i.parent = [](void * u, GTEXT_CSS_Element_Handle e) { T(u)->calls++; return h(T(u)->parent[idx(e)]); };
  i.local_name = [](void * u, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
    const std::string & n = T(u)->names[idx(e)];
    out->data = n.c_str();
    out->length = n.size();
    return !n.empty();
  };
  i.id = [](void * u, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
    const std::string & n = T(u)->ids[idx(e)];
    out->data = n.c_str();
    out->length = n.size();
    return !n.empty();
  };
  i.class_count = [](void * u, GTEXT_CSS_Element_Handle e) { return T(u)->classes[idx(e)].size(); };
  i.class_at = [](void * u, GTEXT_CSS_Element_Handle e, size_t k, GTEXT_CSS_String * out) {
    if (k >= T(u)->classes[idx(e)].size()) {
      return false;
    }
    out->data = T(u)->classes[idx(e)][k].c_str();
    out->length = T(u)->classes[idx(e)][k].size();
    return true;
  };
  i.compare_order = [](void *, GTEXT_CSS_Element_Handle a, GTEXT_CSS_Element_Handle b) { return idx(a) - idx(b); };
  i.state = [](void *, GTEXT_CSS_Element_Handle e) { return static_cast<uint64_t>(idx(e)) & GTEXT_CSS_STATE_HOVER; };
  i.direction = [](void *, GTEXT_CSS_Element_Handle e) { return idx(e) == 2 ? GTEXT_CSS_DIRECTION_RTL : GTEXT_CSS_DIRECTION_LTR; };
  return i;
}

Tree sample() {
  Tree t;
  t.parent = {0, 0, 1, 2};
  t.inh_parent = {0, 0, 1, 1};
  t.names = {"", "html", "body", "div"};
  t.ids = {"", "", "main", ""};
  t.classes = {{}, {}, {"a", "b"}, {}};
  return t;
}

TEST(CssElement, ACallbackTheTableDoesNotHaveIsUnsupportedAndNamesIt) {
  Tree t = sample();
  GTEXT_CSS_Element_Interface table = full_table(t);
  GTEXT_CSS_Element_Result r;
  GTEXT_CSS_Element_Handle out = nullptr;
  // a table with a parent and nothing else
  EXPECT_EQ(gtext_css_element_get_parent(&table, h(3), &out, &r), GTEXT_CSS_OK);
  EXPECT_EQ(idx(out), 2);
  EXPECT_EQ(r.callback, nullptr);
  size_t n = 0;
  EXPECT_EQ(gtext_css_element_get_sibling_index(&table, h(3), &n, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "sibling_index");
  EXPECT_EQ(gtext_css_element_get_sibling_count(&table, h(3), &n, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "sibling_count");
  GTEXT_CSS_String s;
  bool found = true;
  EXPECT_EQ(gtext_css_element_get_namespace_uri(&table, h(3), &s, &found, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "namespace_uri");
  EXPECT_EQ(gtext_css_element_get_lang(&table, h(3), &s, &found, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "lang");
  GTEXT_CSS_Element_Handle prev = nullptr;
  EXPECT_EQ(gtext_css_element_get_previous_sibling(&table, h(3), &prev, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "previous_sibling");
  EXPECT_EQ(gtext_css_element_get_next_sibling(&table, h(3), &prev, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "next_sibling");
  GTEXT_CSS_String name = {"x", 1};
  EXPECT_EQ(gtext_css_element_get_attribute(&table, h(3), nullptr, &name, &s, &found, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "attribute");
  EXPECT_EQ(gtext_css_element_get_parent(nullptr, h(3), &out, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(r.callback, nullptr);
}

TEST(CssElement, EveryCallbackNameIsItsFieldAndAnUnknownNumberHasNone) {
  EXPECT_STREQ(gtext_css_element_callback_name(GTEXT_CSS_ELEMENT_CB_PARENT), "parent");
  EXPECT_STREQ(gtext_css_element_callback_name(GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT), "inheritance_parent");
  EXPECT_STREQ(gtext_css_element_callback_name(GTEXT_CSS_ELEMENT_CB_DIRECTION), "direction");
  EXPECT_STREQ(gtext_css_element_callback_name(GTEXT_CSS_ELEMENT_CB_COUNT), "unknown");
  for (int k = 0; k < GTEXT_CSS_ELEMENT_CB_COUNT; k++) {
    EXPECT_STRNE(gtext_css_element_callback_name(static_cast<GTEXT_CSS_Element_Callback>(k)), "unknown") << k;
  }
}

TEST(CssElement, ATableFromAnOlderHeaderIsSmallerAndWhatIsBeyondItsSizeIsAbsent) {
  Tree t = sample();
  GTEXT_CSS_Element_Interface table = full_table(t);
  EXPECT_TRUE(gtext_css_element_has(&table, GTEXT_CSS_ELEMENT_CB_CLASS_AT));
  // an older caller built the table with the fields up to and including `local_name`
  table.size = offsetof(GTEXT_CSS_Element_Interface, local_name) + sizeof table.local_name;
  EXPECT_TRUE(gtext_css_element_has(&table, GTEXT_CSS_ELEMENT_CB_PARENT));
  EXPECT_TRUE(gtext_css_element_has(&table, GTEXT_CSS_ELEMENT_CB_LOCAL_NAME));
  EXPECT_FALSE(gtext_css_element_has(&table, GTEXT_CSS_ELEMENT_CB_ID)) << "the field is set, and past the size the caller gave";
  EXPECT_FALSE(gtext_css_element_has(&table, GTEXT_CSS_ELEMENT_CB_CLASS_AT));
  GTEXT_CSS_Element_Result r;
  GTEXT_CSS_String s;
  bool found = true;
  EXPECT_EQ(gtext_css_element_get_id(&table, h(2), &s, &found, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "id");
  EXPECT_EQ(gtext_css_element_get_local_name(&table, h(2), &s, &found, &r), GTEXT_CSS_OK);
  EXPECT_TRUE(found);
  EXPECT_EQ(std::string(s.data, s.length), "body");
  // a size too small to hold the user pointer is not a table at all
  table.size = 1;
  EXPECT_EQ(gtext_css_element_check(&table, nullptr, 0, &r), GTEXT_CSS_E_INVALID);
  EXPECT_FALSE(gtext_css_element_has(nullptr, GTEXT_CSS_ELEMENT_CB_PARENT));
  EXPECT_FALSE(gtext_css_element_has(&table, static_cast<GTEXT_CSS_Element_Callback>(99)));
}

TEST(CssElement, CheckNamesTheFirstRequiredCallbackTheTableLacks) {
  Tree t = sample();
  GTEXT_CSS_Element_Interface table = full_table(t);
  GTEXT_CSS_Element_Result r;
  const GTEXT_CSS_Element_Callback need[] = {GTEXT_CSS_ELEMENT_CB_PARENT, GTEXT_CSS_ELEMENT_CB_ID, GTEXT_CSS_ELEMENT_CB_SIBLING_INDEX,
      GTEXT_CSS_ELEMENT_CB_LANG};
  EXPECT_EQ(gtext_css_element_check(&table, need, 2, &r), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_element_check(&table, need, 4, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "sibling_index");
  const GTEXT_CSS_Element_Callback bad[] = {static_cast<GTEXT_CSS_Element_Callback>(77)};
  EXPECT_EQ(gtext_css_element_check(&table, bad, 1, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_element_check(&table, nullptr, 1, &r), GTEXT_CSS_E_INVALID);
}

TEST(CssElement, TheInheritanceParentDefaultsToTheParentAndIsItsOwnWhenTheTableHasOne) {
  Tree t = sample();
  GTEXT_CSS_Element_Interface table = full_table(t);
  GTEXT_CSS_Element_Result r;
  GTEXT_CSS_Element_Handle ip = nullptr;
  // element 3 (div) has the tree parent 2 (body) and, as the inheritance parent, 1 (html): a use clone, or a shadow tree
  EXPECT_EQ(gtext_css_element_get_inheritance_parent(&table, h(3), &ip, &r), GTEXT_CSS_OK);
  EXPECT_EQ(idx(ip), 2) << "no inheritance_parent callback: the parent";
  table.inheritance_parent = [](void * u, GTEXT_CSS_Element_Handle e) { return h(T(u)->inh_parent[idx(e)]); };
  EXPECT_EQ(gtext_css_element_get_inheritance_parent(&table, h(3), &ip, &r), GTEXT_CSS_OK);
  EXPECT_EQ(idx(ip), 1) << "the inheritance parent is not the tree parent";
  GTEXT_CSS_Element_Handle tp = nullptr;
  EXPECT_EQ(gtext_css_element_get_parent(&table, h(3), &tp, &r), GTEXT_CSS_OK);
  EXPECT_EQ(idx(tp), 2);
  // neither callback
  GTEXT_CSS_Element_Interface none;
  std::memset(&none, 0, sizeof none);
  none.size = sizeof none;
  EXPECT_EQ(gtext_css_element_get_inheritance_parent(&none, h(3), &ip, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "parent");
  const GTEXT_CSS_Element_Callback need[] = {GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT};
  EXPECT_EQ(gtext_css_element_check(&none, need, 1, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(gtext_css_element_check(&table, need, 1, &r), GTEXT_CSS_OK);
}

TEST(CssElement, TheCallbacksAnswerWithTheCallersTreeAndAnAbsentThingIsNotFound) {
  Tree t = sample();
  GTEXT_CSS_Element_Interface table = full_table(t);
  GTEXT_CSS_Element_Result r;
  GTEXT_CSS_String s;
  bool found = false;
  ASSERT_EQ(gtext_css_element_get_id(&table, h(2), &s, &found, &r), GTEXT_CSS_OK);
  EXPECT_TRUE(found);
  EXPECT_EQ(std::string(s.data, s.length), "main");
  ASSERT_EQ(gtext_css_element_get_id(&table, h(1), &s, &found, &r), GTEXT_CSS_OK);
  EXPECT_FALSE(found);
  EXPECT_EQ(s.data, nullptr);
  size_t n = 99;
  ASSERT_EQ(gtext_css_element_get_class_count(&table, h(2), &n, &r), GTEXT_CSS_OK);
  EXPECT_EQ(n, 2u);
  ASSERT_EQ(gtext_css_element_get_class_at(&table, h(2), 1, &s, &found, &r), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(s.data, s.length), "b");
  ASSERT_EQ(gtext_css_element_get_class_at(&table, h(2), 2, &s, &found, &r), GTEXT_CSS_OK);
  EXPECT_FALSE(found);
  int order = 7;
  ASSERT_EQ(gtext_css_element_compare_order(&table, h(1), h(3), &order, &r), GTEXT_CSS_OK);
  EXPECT_EQ(order, -1) << "the sign only";
  uint64_t state = 0;
  ASSERT_EQ(gtext_css_element_get_state(&table, h(3), &state, &r), GTEXT_CSS_OK);
  EXPECT_EQ(state, static_cast<uint64_t>(GTEXT_CSS_STATE_HOVER));
  GTEXT_CSS_Direction d = GTEXT_CSS_DIRECTION_LTR;
  ASSERT_EQ(gtext_css_element_get_direction(&table, h(2), &d, &r), GTEXT_CSS_OK);
  EXPECT_EQ(d, GTEXT_CSS_DIRECTION_RTL);
  // a NULL output is an invalid call, and the callback is not called
  t.calls = 0;
  EXPECT_EQ(gtext_css_element_get_parent(&table, h(3), nullptr, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(t.calls, 0);
}

TEST(CssEnvironment, TheDefaultsAreTheDocumentedOnes) {
  const GTEXT_CSS_Environment e = gtext_css_environment_default();
  EXPECT_EQ(e.size, sizeof e);
  EXPECT_EQ(e.default_font_size, 16.0);
  EXPECT_EQ(e.device_pixel_ratio, 1.0);
  EXPECT_EQ(e.color_scheme, GTEXT_CSS_SCHEME_LIGHT);
  EXPECT_EQ(e.large_viewport.width, 1280.0);
  EXPECT_EQ(e.large_viewport.height, 800.0);
  EXPECT_EQ(e.small_viewport.width, 1280.0);
  EXPECT_EQ(e.dynamic_viewport.height, 800.0);
  EXPECT_EQ(e.base_url, nullptr);
  EXPECT_EQ(e.container_size, nullptr);
  EXPECT_EQ(e.font_metric, nullptr);
  EXPECT_EQ(e.normal_line_height, nullptr);
  EXPECT_EQ(e.resolve_color, nullptr);
  EXPECT_EQ(e.resolve_url, nullptr);
  const char * detail = "x";
  EXPECT_EQ(gtext_css_environment_check(&e, &detail), GTEXT_CSS_OK);
  EXPECT_EQ(detail, nullptr);
}

TEST(CssEnvironment, ACheckNamesTheFieldThatIsWrong) {
  const double nan = std::nan("");
  struct Case {
    const char * what;
    void (*edit)(GTEXT_CSS_Environment &);
    const char * mention;
  };
  const Case cases[] = {
    {"zero default size", [](GTEXT_CSS_Environment & e) { e.default_font_size = 0; }, "default_font_size"},
    {"negative default size", [](GTEXT_CSS_Environment & e) { e.default_font_size = -16; }, "default_font_size"},
    {"nan default size", [](GTEXT_CSS_Environment & e) { e.default_font_size = std::nan(""); }, "default_font_size"},
    {"infinite size", [](GTEXT_CSS_Environment & e) { e.default_font_size = HUGE_VAL; }, "default_font_size"},
    {"small viewport", [](GTEXT_CSS_Environment & e) { e.small_viewport.width = 0; }, "small_viewport"},
    {"large viewport", [](GTEXT_CSS_Environment & e) { e.large_viewport.height = -1; }, "large_viewport"},
    {"dynamic viewport", [](GTEXT_CSS_Environment & e) { e.dynamic_viewport.width = std::nan(""); }, "dynamic_viewport"},
    {"pixel ratio", [](GTEXT_CSS_Environment & e) { e.device_pixel_ratio = 0; }, "device_pixel_ratio"},
    {"scheme", [](GTEXT_CSS_Environment & e) { e.color_scheme = static_cast<GTEXT_CSS_Color_Scheme>(5); }, "color_scheme"},
  };
  (void)nan;
  for (const Case & c : cases) {
    GTEXT_CSS_Environment e = gtext_css_environment_default();
    c.edit(e);
    const char * detail = nullptr;
    EXPECT_EQ(gtext_css_environment_check(&e, &detail), GTEXT_CSS_E_INVALID) << c.what;
    ASSERT_NE(detail, nullptr) << c.what;
    EXPECT_NE(std::string(detail).find(c.mention), std::string::npos) << c.what << ": " << detail;
  }
  EXPECT_EQ(gtext_css_environment_check(nullptr, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Environment tiny = gtext_css_environment_default();
  tiny.size = 8;
  EXPECT_EQ(gtext_css_environment_check(&tiny, nullptr), GTEXT_CSS_E_INVALID);
}

} // namespace
