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
 * The `getComputedStyle()` pages of the pinned web-platform-tests `css/cssom/` that need no layout and no script beyond straight-line calls, ported by hand: the page, the test, and the
 * assertion it makes, with the document built from the page's own markup. They are the hand-ported set (the op interpreter of the CSSOM gate does not build a Markup
 * document, so the pages cannot be scored by the runner yet); each test names its page and its title. The pages that ask a laid out box (insets, margins, widths in an iframe),
 * a shadow tree, an iframe, an animation, `::view-transition-*` or the Window object are not here: each needs a laid out box, a script, or a part of the platform that the library does not model.
 */

#include <algorithm>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

using namespace cc;

namespace {

/// `element.style`: the declaration of the element's `style` attribute.
Decl inline_of(Cs & m, const char * selector) {
  GTEXT_CSSOM_Decl * d = nullptr;
  const GTEXT_CSSOM_Attribute_Binding binding = gtext_cssom_markup_inline_binding();
  EXPECT_EQ(gtext_cssom_inline_style(m.d(), m.el(selector), &binding, &d), GTEXT_CSS_OK);
  return Decl(d);
}

/* ---- getComputedStyle-basic.html ----------------------------------------------------------------------------------------------------------------------------- */

const char * kBasic = "<!DOCTYPE html><div id=\"div\">Test</div>";

TEST(CssomWptComputed, BasicSanityCheck) {   // "Sanity check"
  Cs m(kBasic);
  Decl style = inline_of(m, "#div");
  Decl computed = m.computed("#div");
  EXPECT_EQ(text_of(style.d), "") << "Specified CSS text is empty string.";
  EXPECT_EQ(text_of(computed.d), "") << "Computed CSS text is empty string.";
  EXPECT_EQ(get(style.d, "background-color"), "") << "Specified property value is empty string";
  EXPECT_EQ(get(computed.d, "background-color"), "rgba(0, 0, 0, 0)") << "Computed property value is resolved initial value.";
  Decl again = m.computed("#div");
  EXPECT_NE(computed.d, again.d) << "getComputedStyle() creates new instance each time.";
}

TEST(CssomWptComputed, BasicCssText) {   // "cssText"
  Cs m(kBasic);
  Decl style = inline_of(m, "#div");
  EXPECT_EQ(set_text(style.d, "color: green;").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(style.d), "color: green;") << "CSS text of specified value is not empty.";
  Decl computed = m.computed("#div");
  EXPECT_EQ(text_of(computed.d), "") << "CSS text of computed value is always empty string.";
}

TEST(CssomWptComputed, BasicSetPropertyAndRemovePropertyAreNoModificationAllowed) {   // "setProperty", "removeProperty"
  Cs m(kBasic);
  Decl computed = m.computed("#div");
  EXPECT_EQ(set(computed.d, "color", "red").st, GTEXT_CSS_E_NO_MODIFICATION) << "Modifying computed value is not allowed.";
  EXPECT_EQ(remove(computed.d, "color").st, GTEXT_CSS_E_NO_MODIFICATION) << "Modifying property is not allowed.";
  EXPECT_STREQ(gtext_cssom_exception_name(GTEXT_CSS_E_NO_MODIFICATION), "NoModificationAllowedError");
}

/* ---- computed-style-001.html (read_only) ---------------------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, ComputedStyle001ReadOnly) {   // "read_only"
  Cs m("<!DOCTYPE html><html><head><style>#outside{width:200px;height:200px} #outside div{font-size:100px} #inside{width:50%;height:100px}</style></head>"
       "<body><div id=\"outside\"><div id=\"inside\"></div></div></body></html>");
  Decl inner = m.computed("#inside");
  EXPECT_EQ(set_text(inner.d, "color: blue;").st, GTEXT_CSS_E_NO_MODIFICATION) << "do not allow setting cssText on a readonly CSSStyleDeclaration";
  EXPECT_EQ(set(inner.d, "color", "blue").st, GTEXT_CSS_E_NO_MODIFICATION) << "do not allow calling setProperty on a readonly CSSStyleDeclaration";
  GTEXT_CSSOM_Outcome o;
  EXPECT_EQ(gtext_cssom_decl_idl_set(inner.d, "color", 5, "blue", 4, &o), GTEXT_CSS_E_NO_MODIFICATION) << "do not allow setting a property on a readonly CSSStyleDeclaration";
  EXPECT_EQ(get(inner.d, "font-size"), "100px") << "and the inherited font size is the resolved one";
}

/* ---- getComputedStyle-root.html, getComputedStyle-root-variables.html ----------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, RootVariables) {   // "Accessing variables in :root pseudo-selector"
  Cs m("<!DOCTYPE html><html><head><style>:root { --a: red; }</style></head><body></body></html>");
  Decl d = m.computed("html");
  EXPECT_EQ(get(d.d, "--a"), "red");
}

TEST(CssomWptComputed, RootVariablesAreInheritedByTheBody) {   // "getComputedStyle should return CSS variables defined on :root"
  Cs m("<!DOCTYPE html><html><head><style>:root { --test-var: red; }</style></head><body></body></html>");
  Decl d = m.computed("body");
  EXPECT_EQ(get(d.d, "--test-var"), "red");
}

/* ---- getComputedStyle-important.html -------------------------------------------------------------------------------------------------------------------------- */

const char * kImportant =
    "<!DOCTYPE html><html><head><style>#testElement { display: none !important; }</style><style>#testElement2 { display: none !important; }</style>"
    "<style>#testElement2 { display: inline-block; }</style></head><body><div id=\"testElement\" style=\"display: block;\"></div><div id=\"testElement2\"></div></body></html>";

TEST(CssomWptComputed, ImportantStylesheetValueBeatsTheInlineValue) {   // "getComputedStyle should return !important stylesheet value over inline style"
  Cs m(kImportant);
  EXPECT_EQ(m.value("#testElement", "display"), "none");
}

TEST(CssomWptComputed, ImportantStylesheetValueBeatsTheSubsequentStylesheetValue) {   // "getComputedStyle should return !important stylesheet value rather than the subsequent stylesheet value"
  Cs m(kImportant);
  EXPECT_EQ(m.value("#testElement2", "display"), "none");
}

/* ---- getComputedStyle-live.html ---------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, TheDeclarationIsLive) {   // "getComputedStyle() returns live values"
  Cs m("<!DOCTYPE html><title>t</title><div id=\"div\"></div>");
  Decl computed = m.computed("#div");
  EXPECT_EQ(get(computed.d, "background-color"), "rgba(0, 0, 0, 0)") << "before";
  Decl style = inline_of(m, "#div");
  EXPECT_EQ(set(style.d, "background-color", "red").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(computed.d, "background-color"), "rgb(255, 0, 0)") << "after";
}

/* ---- getComputedStyle-mutations.html ---------------------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, UpdatingTheDocumentInvalidatesTheStyleCache) {   // the nine tests of the page, in order, on one document
  Cs m("<!DOCTYPE html><html><head><style>"
       ".my-link { font-size: 17px; } .my-link[href] { font-size: 18px; } .my-link[href=\"#a\"] { font-size: 19px; } .my-link[href=\"#b\"] { font-size: 20px; }"
       ".my-box ~ .my-link { font-size: 21px; } .my-box.bigger ~ .my-link { font-size: 22px; } .my-link:empty { font-size: 23px; }"
       "</style></head><body><a class=\"my-link\">Hello world</a></body></html>");
  GTEXT_Markup_Node * link = m.el(".my-link");
  auto size = [&] { return get(m.computed(".my-link").d, "font-size"); };
  EXPECT_EQ(size(), "17px") << "Initial style value";
  set_attr(link, "href", "#x");
  EXPECT_EQ(size(), "18px") << "Assigning to attribute updates the computed style";
  set_attr(link, "href", "#a");
  EXPECT_EQ(size(), "19px") << "Setting an attribute updates the computed style";
  set_attr(link, "href", "#b");
  EXPECT_EQ(size(), "20px") << "Replacing an attribute updates the computed style";
  EXPECT_TRUE(gtext_markup_element_remove_attribute(link, nullptr, "href"));
  EXPECT_EQ(size(), "17px") << "Removing an attribute updates the computed style";
  GTEXT_Markup_Node * box = make(m.markup(), "div");
  set_attr(box, "class", "my-box");
  ASSERT_EQ(gtext_markup_insert_before(gtext_markup_node_parent(link), box, link), GTEXT_MARKUP_OK);
  EXPECT_EQ(size(), "21px") << "Inserting an element updates the computed style";
  GTEXT_Markup_Node * bigger = make(m.markup(), "div");
  set_attr(bigger, "class", "my-box bigger");
  ASSERT_EQ(gtext_markup_insert_before(gtext_markup_node_parent(link), bigger, box), GTEXT_MARKUP_OK);
  detach(box);
  gtext_markup_node_free(box);
  EXPECT_EQ(size(), "22px") << "Replacing an element updates the computed style";
  detach(bigger);
  gtext_markup_node_free(bigger);
  EXPECT_EQ(size(), "17px") << "Removing an element updates the computed style";
  while (GTEXT_Markup_Node * c = gtext_markup_node_first_child(link)) {
    detach(c);
    gtext_markup_node_free(c);
  }
  EXPECT_EQ(size(), "23px") << "Emptying an element updates the computed style";
}

/* ---- getComputedStyle-style-element-updates.html --------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, UpdatingAStyleElementChangesTheResults) {   // the four tests of the page, in order
  Cs m("<!DOCTYPE html><html><head><style>.my-class { font-size: 24px; }</style></head><body><h1 class=\"my-class\">Hello world</h1></body></html>");
  auto size = [&] { return get(m.computed("h1").d, "font-size"); };
  EXPECT_EQ(size(), "24px") << "Sanity check: getComputedStyle works as expected";
  GTEXT_Markup_Node * inserted = make_style(m.markup(), ".my-class { font-size: 32px; }");
  append(m.el("head"), inserted);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(size(), "32px") << "Inserting a new CSS element that overrides the old one updates the computed style";
  set_text(m.markup(), inserted, ".my-class { font-size: 48px; }");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(size(), "48px") << "Updating the inserted CSS element changes the computed style";
  detach(inserted);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  gtext_markup_node_free(inserted);
  EXPECT_EQ(size(), "24px") << "Removing the inserted CSS element updates the computed style back to original value";
}

/* ---- getComputedStyle-display-none-001.html .. -003.html ------------------------------------------------------------------------------------------------------ */

TEST(CssomWptComputed, DisplayNoneSubtreeIsInvalidatedByAnInheritedChange) {   // 001, "...due to inherited changes to an ancestor"
  Cs m("<!doctype html><html><head><style>#undisplayed, #host { display: none; color: red; }</style></head><body><div id=\"undisplayed\"><div id=\"child\"></div></div><div id=\"host\"></div></body></html>");
  Decl undisplayed = m.computed("#undisplayed");
  Decl child = m.computed("#child");
  EXPECT_EQ(get(undisplayed.d, "color"), "rgb(255, 0, 0)");
  EXPECT_EQ(get(child.d, "color"), "rgb(255, 0, 0)");
  Decl style = inline_of(m, "#undisplayed");
  EXPECT_EQ(set(style.d, "color", "green").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(undisplayed.d, "color"), "rgb(0, 128, 0)");
  EXPECT_EQ(get(child.d, "color"), "rgb(0, 128, 0)");
}

TEST(CssomWptComputed, DisplayNoneSubtreeIsInvalidatedByRuleMatchingChanges) {   // 002
  Cs m("<!doctype html><html><head><style>#undisplayed { display: none; color: red; } .sibling + #undisplayed { color: green; } .sibling + #undisplayed > div { color: yellow; }</style></head>"
       "<body><div id=\"helper\"></div><div id=\"undisplayed\"><div id=\"child\"></div></div></body></html>");
  Decl undisplayed = m.computed("#undisplayed");
  Decl child = m.computed("#child");
  EXPECT_EQ(get(undisplayed.d, "color"), "rgb(255, 0, 0)");
  EXPECT_EQ(get(child.d, "color"), "rgb(255, 0, 0)");
  set_attr(m.el("#helper"), "class", "sibling");
  EXPECT_EQ(get(undisplayed.d, "color"), "rgb(0, 128, 0)");
  EXPECT_EQ(get(child.d, "color"), "rgb(255, 255, 0)");
}

TEST(CssomWptComputed, DisplayNoneSubtreeIsInvalidatedByAttributeMutations) {   // 003
  Cs m("<!doctype html><html><head><style>div { color: red } .foo { color: lime; }</style></head><body><div style=\"display: none\"><div id=\"undisplayed\"></div></div></body></html>");
  Decl undisplayed = m.computed("#undisplayed");
  EXPECT_EQ(get(undisplayed.d, "color"), "rgb(255, 0, 0)");
  set_attr(m.el("#undisplayed"), "class", "foo");
  EXPECT_EQ(get(undisplayed.d, "color"), "rgb(0, 255, 0)");
}

/* ---- getComputedStyle-visibility-nominal-case.html, -override.html, -unset.html ------------------------------------------------------------------------ */

TEST(CssomWptComputed, Visibility) {
  Cs nominal("<!DOCTYPE html><html><head><style>#hiddenDiv { visibility: hidden; } * { margin-bottom: 10px; }</style></head><body><div id=\"hiddenDiv\">Hello, Ronron!</div></body></html>");
  EXPECT_EQ(nominal.value("body", "visibility"), "visible") << "Sanity check: getComputedStyle returns apparent visibility";
  EXPECT_EQ(nominal.value("#hiddenDiv", "visibility"), "hidden") << "Real check: getComputedStyle returns hidden visibility";
  Cs over("<!DOCTYPE html><html><head><style>#parent { visibility: hidden; } #child { visibility: visible; }</style></head><body><div id=\"parent\"><div id=\"child\">Hello, Dave!</div></div></body></html>");
  EXPECT_EQ(over.value("#parent", "visibility"), "hidden");
  EXPECT_EQ(over.value("#child", "visibility"), "visible") << "computed visibility can be visible even if the parent is not";
  Cs unset("<!DOCTYPE html><html><head><style>#parent { visibility: hidden; } #child { visibility: visible; } div#child { visibility: unset; }</style></head>"
           "<body><div id=\"parent\"><div id=\"child\">Hello, Dave!</div></div></body></html>");
  EXPECT_EQ(unset.value("#parent", "visibility"), "hidden");
  EXPECT_EQ(unset.value("#child", "visibility"), "hidden") << "computed visibility gets unset and inherits from parent";
}

/* ---- getComputedStyle-property-order.html, getComputedStyle-logical-enumeration.html ------------------------------------------------------------------------ */

TEST(CssomWptComputed, PropertiesAreSortedLexicographicallyVendorPrefixedLast) {   // "Computed style properties should be sorted lexicographically"
  Cs m("<!DOCTYPE html><html><head></head><body></body></html>");
  Decl d = m.computed("html");
  const std::vector<std::string> properties = names(d.d);
  std::vector<std::string> sorted = properties;
  std::sort(sorted.begin(), sorted.end(), [](const std::string & a, const std::string & b) {
    if ((a[0] == '-') != (b[0] == '-')) {   // "Sort vendor-prefixed properties after standard ones"
      return b[0] == '-';
    }
    return a < b;
  });
  EXPECT_EQ(properties, sorted);
  EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end()) << "There shouldn't be repeated properties";
}

TEST(CssomWptComputed, LogicalPropertiesInTheEnumeration) {   // "Logical properties in enumeration of computed style"
  Cs m("<!DOCTYPE html><html><head></head><body></body></html>");
  Decl d = m.computed("html");
  const std::vector<std::string> properties = names(d.d);
  auto has = [&](const char * name) { return std::find(properties.begin(), properties.end(), name) != properties.end(); };
  EXPECT_TRUE(has("block-size")) << "Logical longhands should be enumerated in computed style";
  EXPECT_FALSE(has("padding-block")) << "Logical shorthands should not be enumerated in computed style";
  EXPECT_FALSE(has("safe-area-inset-top")) << "Environment variables should not be enumerated in computed style";
}

/* ---- getComputedStyle-detached-subtree.html --------------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, NoStyleForADetachedElementOrOneOutsideTheFlatTree) {   // "getComputedStyle returns no style for ..."
  std::set<const void *> outside;
  Cs m("<!DOCTYPE html><html><body><div id=\"host\"><div id=\"non-slotted\"><div id=\"non-slotted-descendant\"></div></div></div></body></html>", [&](GTEXT_CSSOM_Config & c) {
    c.element_state = [](void * user, const void * element) -> uint32_t {
      return static_cast<std::set<const void *> *>(user)->count(element) ? GTEXT_CSSOM_ELEMENT_CONNECTED : GTEXT_CSSOM_ELEMENT_STATE_ALL;
    };
    c.element_state_user = &outside;
  });
  GTEXT_Markup_Node * detached = make(m.markup(), "div");
  for (const void * h : {static_cast<const void *>(gtext_css_markup_handle(detached))}) {
    outside.insert(h);   // the caller's hook says what the tree is: a detached element is not connected
  }
  auto none = [&](const void * h, const char * what) {
    GTEXT_CSSOM_Decl * d = nullptr;
    ASSERT_EQ(gtext_cssom_computed_style(m.d(), h, nullptr, 0, &d), GTEXT_CSS_OK) << what;
    Decl hold(d);
    EXPECT_EQ(length(d), 0u) << what;
    EXPECT_EQ(get(d, "color"), "") << what;
  };
  none(gtext_css_markup_handle(detached), "detached element");
  outside.insert(m.handle("#non-slotted"));
  outside.insert(m.handle("#non-slotted-descendant"));
  none(m.handle("#non-slotted"), "element outside the flat tree");
  none(m.handle("#non-slotted-descendant"), "descendant outside the flat tree");
  gtext_markup_node_free(detached);
  // and without a hook, a Markup element whose top is not the document element is not connected
  Cs plain("<!DOCTYPE html><html><body><div id=\"d\"></div></body></html>");
  GTEXT_Markup_Node * loose = make(plain.markup(), "div");
  GTEXT_CSSOM_Decl * d = nullptr;
  ASSERT_EQ(gtext_cssom_computed_style(plain.d(), gtext_css_markup_handle(loose), nullptr, 0, &d), GTEXT_CSS_OK);
  Decl hold(d);
  EXPECT_EQ(length(d), 0u) << "getComputedStyle returns no style for detached element";
  gtext_markup_node_free(loose);
}

/* ---- getComputedStyle-resolved-colors.html ---------------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, TheResolvedValueOfTheColorPropertiesIsTheUsedValue) {
  // `Menu` is a system color: the used value is the user agent's, which only the environment knows. The page's user agent has one; this test gives the context a palette of one.
  struct Palette {
    static bool resolve(void *, const GTEXT_CSS_Color *, GTEXT_CSS_Color_Scheme, double rgba[4]) {
      rgba[0] = 240;
      rgba[1] = 240;
      rgba[2] = 240;
      rgba[3] = 1;
      return true;
    }
  };
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.resolve_color = &Palette::resolve;
  Cs m("<!DOCTYPE html><html><head><style>#target { background-color: Menu; border: 1px solid Menu; box-shadow: 1px 1px Menu; caret-color: Menu; color: Menu; outline-color: Menu; }</style></head>"
       "<body><div id=\"target\"></div></body></html>");
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = gtext_css_markup_interface(m.page->adapter);
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  ASSERT_EQ(gtext_css_compute_context_new(R(), &env, &co, &ctx, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_set_compute_context(m.d(), ctx), GTEXT_CSS_OK);
  Decl d = m.computed("#target");
  for (const char * property : {"background-color", "border-block-end-color", "border-block-start-color", "border-bottom-color", "border-inline-end-color", "border-inline-start-color",
           "border-left-color", "border-right-color", "border-top-color", "box-shadow", "caret-color", "color", "outline-color"}) {
    const std::string v = get(d.d, property);
    EXPECT_TRUE(v.rfind("rgb", 0) == 0 || v.find("rgb(240, 240, 240)") != std::string::npos) << property << ": " << v;
  }
  EXPECT_EQ(gtext_cssom_document_set_compute_context(m.d(), nullptr), GTEXT_CSS_OK);
  gtext_css_compute_context_free(ctx);
}

/* ---- getComputedStyle-resolved-min-size-auto.html ----------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, TheResolvedValueOfAutoMinWidthAndMinHeightIsZeroExceptInTheDefinedCases) {   // one test for each element of the page
  Cs m("<!DOCTYPE html><html><body><div style=\"overflow: scroll; width: 300px; height: 200px\">"
       "<div id=\"block-box\"></div><div id=\"inline-box\"></div>"
       "<div id=\"block-with-valid-aspect-ratio\" style=\"aspect-ratio: 1/1\" preservesAuto></div>"
       "<div id=\"block-with-degenerate-aspect-ratio\" style=\"aspect-ratio: 0/1\" preservesAuto></div>"
       "<div id=\"block-with-two-part-valid-aspect-ratio\" style=\"aspect-ratio: auto 1/1\" preservesAuto></div>"
       "<div id=\"block-with-two-part-degenerate-aspect-ratio\" style=\"aspect-ratio: auto 0/1\" preservesAuto></div>"
       "<span id=\"inline-with-valid-aspect-ratio\" style=\"aspect-ratio: 1/1\" preservesAuto></span>"
       "<span id=\"inline-with-degenerate-aspect-ratio\" style=\"aspect-ratio: 0/1\" preservesAuto></span>"
       "<div style=\"display: flex\"><div id=\"flex-item-row\" preservesAuto></div></div>"
       "<div style=\"display: flex; flex-direction: column;\"><div id=\"flex-item-column\" preservesAuto></div></div>"
       "<div style=\"display: grid\"><div id=\"grid-item\" preservesAuto></div></div>"
       "<div id=\"display-none\" style=\"display: none\"></div>"
       "<div id=\"display-none-valid-aspect-ratio\" style=\"display: none; aspect-ratio:2/1\"></div>"
       "<div style=\"display:none\"><div id=\"display-none-subtree-valid-aspect-ratio\" style=\"aspect-ratio:2/1\"></div></div>"
       "<div style=\"display: none\"><div style=\"display: flex\"><div id=\"display-none-subtree-flex-item\"></div></div></div>"
       "<div style=\"display: none\"><div style=\"display: grid\"><div id=\"display-none-subtree-grid-item\"></div></div></div>"
       "</div></body></html>");
  size_t count = 0;
  for (GTEXT_Markup_Node * e : m.page->all("[id]")) {
    count++;
    GTEXT_Markup_Attribute attr;
    std::string id;
    ASSERT_TRUE(gtext_markup_element_get_attribute(e, nullptr, "id", &attr));
    id.assign(attr.value, attr.value_length);
    const bool preserves = gtext_markup_element_get_attribute(e, nullptr, "preservesauto", &attr);
    const std::string expected = preserves ? "auto" : "0px";
    Decl cs = [&] {
      GTEXT_CSSOM_Decl * d = nullptr;
      EXPECT_EQ(gtext_cssom_computed_style(m.d(), gtext_css_markup_handle(e), nullptr, 0, &d), GTEXT_CSS_OK);
      return Decl(d);
    }();
    GTEXT_CSSOM_Decl * style = nullptr;
    const GTEXT_CSSOM_Attribute_Binding binding = gtext_cssom_markup_inline_binding();
    ASSERT_EQ(gtext_cssom_inline_style(m.d(), e, &binding, &style), GTEXT_CSS_OK);
    Decl inline_style(style);
    for (const char * prop : {"min-width", "min-height"}) {
      EXPECT_EQ(get(cs.d, prop), expected) << id << ": '" << prop << ":auto' (as initial value)";
      EXPECT_EQ(set(inline_style.d, prop, "auto").st, GTEXT_CSS_OK);
      EXPECT_EQ(get(cs.d, prop), expected) << id << ": '" << prop << ":auto' (as specified value)";
      EXPECT_EQ(remove(inline_style.d, prop).st, GTEXT_CSS_OK);
    }
  }
  EXPECT_EQ(count, 16u);
}

/* ---- resolved-border-width.html -------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, BorderWidthsReturnComputedRatherThanUsedValues) {   // "checkbox", "radio" (without the form control's own style)
  Cs m("<!DOCTYPE html><html><body><input type=checkbox style=\"border: 5px solid red\"><input type=radio style=\"border: 5px solid red\"></body></html>");
  for (const char * sel : {"input[type=checkbox]", "input[type=radio]"}) {
    Decl d = m.computed(sel);
    for (const char * prop : {"border-width", "border-top-width", "border-right-width", "border-bottom-width", "border-left-width"}) {
      EXPECT_EQ(get(d.d, prop), "5px") << sel << " " << prop;
    }
  }
}

/* ---- getComputedStyle-pseudo-with-argument.html ---------------------------------------------------------------------------------------------------------- */

TEST(CssomWptComputed, PseudoElementsWithArguments) {   // the parsable and the non-parsable lists of the page that this library's selector grammar decides alike
  Cs m("<!doctype html><html><head><style>#pseudo-invalid::highlight(name) { color: rgb(0, 128, 0); }</style></head><body><ul><li id=\"pseudo-invalid\">Item</li></ul></body></html>");
  for (const char * bad : {"::before(test)", "::highlight", "::highlight(", "::highlight()", "::highlight(1)", "::highlight($)", "::highlight (name)", "::highlight(name)a", ":highlight(name)",
           ":view-transition-group(name)", ":view-transition-image-pair(name)", ":view-transition-old(name)", ":view-transition-new(name)"}) {
    Decl d = m.computed("li", bad);
    EXPECT_EQ(length(d.d), 0u) << "This pseudo-element should not parse: " << bad;
  }
  for (const char * good : {"::highlight(name)", "::highlight(\nname", "::highlight(name\t", "::highlight( name ", "::highlight( n\\61me )"}) {
    Decl d = m.computed("li", good);
    EXPECT_NE(length(d.d), 0u) << "This pseudo-element should parse: " << good;
    EXPECT_EQ(get(d.d, "color"), "rgb(0, 128, 0)") << good;
  }
}

}  // namespace
