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
 * `getComputedStyle()` and the resolved values (css_cssom_computed.h), against the served text of the CSS Object Model (read 2026-10-30; section 7.2 "Extensions to the Window
 * Interface" and section 9 "Resolved Values"), CSS Transforms 1 and 2, CSS Grid 2, CSS Animations 2, CSS Sizing 3, CSS Text 3 and CSS Borders 4. Each test names the sentence it follows.
 */

#include <algorithm>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

using namespace cc;

namespace {

const char * kPage =
    "<!DOCTYPE html><html><head><style id=s>"
    "div{color:rgb(10,20,30);margin-top:7px} p{width:50px;line-height:2;border-top-style:solid;border-top-width:3px;background-color:currentcolor} .x{color:blue}"
    "</style></head><body><div id=d><p id=a class=lead>x</p><p id=b>y</p></div><span id=s1>z</span></body></html>";

/* ---- the first steps ------------------------------------------------------------------------------------------------------------------------------------- */

/* CSSOM, 7.2: "If obj is not null, and elt is connected, part of the flat tree, and its shadow-including root has a browsing context which either doesn't have a browsing context
   container, or whose browsing context container is being rendered, set decls to a list of all longhand properties that are supported CSS properties". */
TEST(CssomComputed, ARenderedElementHasEverySupportedLonghandAndNothingElse) {
  Cs m(kPage);
  Decl d = m.computed("#a");
  const size_t n = length(d.d);
  EXPECT_GT(n, 400u) << "the longhands of the registry";
  bool empty = true;
  EXPECT_EQ(gtext_cssom_computed_is_empty(d.d, &empty), GTEXT_CSS_OK);
  EXPECT_FALSE(empty);
  EXPECT_EQ(gtext_cssom_decl_owner(d.d), GTEXT_CSSOM_DECL_COMPUTED);
  EXPECT_TRUE(gtext_cssom_decl_readonly(d.d));
  for (const std::string & name : names(d.d)) {
    GTEXT_CSS_Property_Info info;
    const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(R(), name.data(), name.size());
    ASSERT_NE(id, 0u) << name;
    ASSERT_TRUE(gtext_css_registry_property_info(R(), id, &info));
    EXPECT_FALSE(info.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS)) << name << " is not a longhand that is not an alias";
  }
}

/* The same step: "in lexicographical order". Custom properties come after, "all the custom properties whose computed value for obj is not the guaranteed-invalid value". */
TEST(CssomComputed, TheLonghandsAreInLexicographicOrderAndTheCustomPropertiesFollow) {
  Cs m("<!DOCTYPE html><html><head><style>#a{--b:2; --a:1; --c:var(--missing)}</style></head><body><p id=a>x</p></body></html>");
  Decl d = m.computed("#a");
  const std::vector<std::string> all = names(d.d);
  size_t first_custom = all.size();
  for (size_t i = 0; i < all.size(); i++) {
    if (all[i].rfind("--", 0) == 0) {
      first_custom = i;
      break;
    }
  }
  ASSERT_LT(first_custom, all.size());
  EXPECT_TRUE(std::is_sorted(all.begin() + first_custom, all.end())) << "the custom properties";
  EXPECT_EQ(std::vector<std::string>(all.begin() + first_custom, all.end()), (std::vector<std::string>{"--a", "--b"})) << "--c is the guaranteed-invalid value";
  EXPECT_EQ(get(d.d, "--a"), "1");
  /* the longhands whose names begin with a hyphen come after the others: both browsers list them so (the served text says only "lexicographical order") */
  size_t first_dash = first_custom;
  for (size_t i = 0; i < first_custom; i++) {
    if (all[i][0] == '-') {
      first_dash = i;
      break;
    }
  }
  EXPECT_LT(first_dash, first_custom) << "there are vendor-prefixed longhands";
  EXPECT_GT(first_dash, 100u) << "and they come after the others";
  for (size_t i = first_dash; i < first_custom; i++) {
    EXPECT_EQ(all[i][0], '-') << all[i] << " after a vendor-prefixed name";
  }
  EXPECT_TRUE(std::is_sorted(all.begin(), all.begin() + first_dash));
  EXPECT_TRUE(std::is_sorted(all.begin() + first_dash, all.begin() + first_custom));
  EXPECT_EQ(get(d.d, "--b"), "2");
  EXPECT_EQ(get(d.d, "--c"), "");
}

TEST(CssomComputed, AnElementThatIsNotConnectedInTheFlatTreeOrRenderedHasNoDeclarations) {
  std::set<const void *> down;
  for (unsigned bits : {0u, 1u, 2u, 4u, 3u, 5u, 6u}) {
    SCOPED_TRACE(bits);
    struct S {
      static uint32_t state(void * user, const void *) {
        return *static_cast<uint32_t *>(user);
      }
    };
    uint32_t flags = bits;
    Cs m(kPage, [&](GTEXT_CSSOM_Config & c) {
      c.element_state = &S::state;
      c.element_state_user = &flags;
    });
    Decl d = m.computed("#a");
    EXPECT_EQ(length(d.d), 0u);
    EXPECT_EQ(get(d.d, "color"), "");
    bool empty = false;
    EXPECT_EQ(gtext_cssom_computed_is_empty(d.d, &empty), GTEXT_CSS_OK);
    EXPECT_TRUE(empty);
    flags = GTEXT_CSSOM_ELEMENT_STATE_ALL;
    gtext_cssom_document_tree_changed(m.d());   // the state is a fact of the tree: the declaration is live, it reads again
    EXPECT_GT(length(d.d), 400u) << "live: when the facts hold, the same object has the values";
  }
}

TEST(CssomComputed, AnUnrenderedElementAccordingToTheLayoutIsEmptyAndADisplayNoneElementIsNot) {
  Layout lay;
  Cs m("<!DOCTYPE html><html><head><style>.gone{display:none} p{color:red}</style></head><body><p id=a class=gone>x</p><p id=b>y</p></body></html>",
      [&](GTEXT_CSSOM_Config & c) { lay.install(c); });
  Decl none = m.computed("#a");
  EXPECT_EQ(get(none.d, "display"), "none") << "a display: none element is connected and in the tree, so it has its computed values";
  EXPECT_EQ(get(none.d, "color"), "rgb(255, 0, 0)");
  lay.unrendered.insert(m.handle("#b"));
  Decl b = m.computed("#b");
  EXPECT_EQ(length(b.d), 0u) << "the layout says the element is not being rendered";
}

/* CSSOM, 7.2: "If pseudoElt is provided, is not the empty string, and starts with a colon, then: Parse pseudoElt as a <pseudo-element-selector>, and let type be the result. If type is
   failure, or is a ::slotted() or ::part() pseudo-element, let obj be null." and "Otherwise let obj be the given pseudo-element of elt."; "Note: CSS2 pseudo-elements should match both the
   double and single-colon versions." */
TEST(CssomComputed, APseudoElementArgumentThatIsNotOneIsEmptyAndAStringWithoutAColonIsTheElement) {
  Cs m("<!DOCTYPE html><html><head><style>p{color:red} p::before{color:blue} p::after{color:green}</style></head><body><p id=a>x</p></body></html>");
  auto color = [&](const char * pseudo) {
    Decl d = m.computed("#a", pseudo);
    return get(d.d, "color");
  };
  EXPECT_EQ(color(nullptr), "rgb(255, 0, 0)");
  EXPECT_EQ(color(""), "rgb(255, 0, 0)") << "the empty string is not a pseudo-element";
  EXPECT_EQ(color("before"), "rgb(255, 0, 0)") << "no colon: read as no pseudo-element";
  EXPECT_EQ(color("::before"), "rgb(0, 0, 255)");
  EXPECT_EQ(color(":before"), "rgb(0, 0, 255)") << "the single-colon version of a CSS2 pseudo-element";
  EXPECT_EQ(color("::after"), "rgb(0, 128, 0)");
  EXPECT_EQ(color(":after"), "rgb(0, 128, 0)");
  for (const char * bad : {"::slotted(p)", "::part(x)", "::bogus", ":hover", ":", "::", "::before::after", "::before, ::after", ":is(p)", "::before extra", "::before ", "::before\n"}) {
    SCOPED_TRACE(bad);
    Decl d = m.computed("#a", bad);
    EXPECT_EQ(length(d.d), 0u) << "obj is null";
    bool empty = false;
    EXPECT_EQ(gtext_cssom_computed_is_empty(d.d, &empty), GTEXT_CSS_OK);
    EXPECT_TRUE(empty);
  }
}

/* Pseudo-Elements 4, "Styling the ::first-line Pseudo-element": "The following CSS properties apply to a ::first-line pseudo-element: all font properties ... the 'color' and 'opacity'
   properties ... all background properties ... any typesetting properties that apply to inline elements ... all text decoration properties", and "User agents may apply other properties
   as well except for the following excluded properties: writing-mode, direction, text-orientation"; "Styling the First-Letter Pseudo-elements": "margin and padding properties" and "border
   properties and 'box-shadow'"; Lists 3, "Properties Applying to ::marker": "only the following CSS properties actually apply to a marker box: the 'text-combine-upright', 'unicode-bidi',
   and 'direction' properties, the 'content' property, all animation and transition properties" and "Inheritable properties that apply to text can be set on the ::marker pseudo-element".
   A declaration of a property that does not apply takes no part in the cascade of the pseudo-element: both browsers answer the inherited or initial value (css_pseudo_answers.jsonl). */
TEST(CssomComputed, APropertyThatDoesNotApplyToAPseudoElementIsLeftOutOfItsCascade) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "p::first-line, p::first-letter, li::marker, p::before { position: absolute; margin-left: 10px; color: rgb(10, 20, 30); transition: transform 1s; word-break: break-all; float: right; display: inline-block;"
       " text-decoration-line: underline; text-decoration-skip-ink: none; list-style-type: square; direction: rtl; cursor: move; border-top-style: dashed; }"
       "</style></head><body><p id=a>x</p><ul><li id=b>y</li></ul></body></html>");
  auto read = [&](const char * element, const char * pseudo, const char * property) {
    Decl d = m.computed(element, pseudo);
    return get(d.d, property);
  };
  for (const char * pe : {"::first-line", "::first-letter", "::marker"}) {
    SCOPED_TRACE(pe);
    const char * e = std::string(pe) == "::marker" ? "#b" : "#a";
    EXPECT_EQ(read(e, pe, "color"), "rgb(10, 20, 30)") << "color is a font and color property: it applies";
    EXPECT_EQ(read(e, pe, "position"), "static") << "position does not apply to any of the three";
    EXPECT_EQ(read(e, pe, "display"), std::string(pe) == "::first-letter" ? "block" : "inline")
        << "nor does display: the initial value, the library having no user agent sheet (the float of ::first-letter blockifies it)";
  }
  EXPECT_EQ(read("#a", "::first-line", "margin-left"), "0px") << "margin does not apply to ::first-line";
  EXPECT_EQ(read("#a", "::first-letter", "margin-left"), "10px") << "but it applies to ::first-letter";
  EXPECT_EQ(read("#a", "::first-letter", "border-top-style"), "dashed") << "and so does a border";
  EXPECT_EQ(read("#a", "::first-line", "border-top-style"), "none");
  EXPECT_EQ(read("#a", "::first-line", "transition-property"), "all") << "no transition applies to ::first-line";
  EXPECT_EQ(read("#b", "::marker", "transition-property"), "transform") << "all animation and transition properties apply to ::marker";
  EXPECT_EQ(read("#a", "::first-line", "word-break"), "break-all") << "a typesetting property of CSS Text is listed, so it stays applied";
  EXPECT_EQ(read("#a", "::first-line", "direction"), "ltr") << "the excluded properties";
  EXPECT_EQ(read("#b", "::marker", "direction"), "rtl") << "direction is listed for ::marker";
  EXPECT_EQ(read("#a", "::first-letter", "float"), "right") << "float applies to ::first-letter";
  EXPECT_EQ(read("#a", "::first-line", "float"), "none");
  EXPECT_EQ(read("#b", "::marker", "text-decoration-line"), "none") << "a property that is not inherited and not listed does not apply to a marker";
  EXPECT_EQ(read("#b", "::marker", "list-style-type"), "disc") << "the list properties belong to the list item";
  EXPECT_EQ(read("#b", "::marker", "text-decoration-skip-ink"), "none")
      << "an inherited property that applies to text can be set on ::marker (Lists 3); Chromium applies it and Firefox leaves it, the served text decides";
  EXPECT_EQ(read("#a", "::before", "position"), "absolute") << "the other pseudo-elements have every property";
}

/* Pseudo-Elements 4, "Cascading and Per-Element Highlight Styles": "When any supported property is not given a value by the cascade, or given a value of inherit or unset, its specified value is
   determined by inheritance from the corresponding highlight pseudo-element of its originating element's parent element. This occurs regardless of whether that property is an inherited
   property." and "Additionally, for highlight pseudo-elements originating from the root element the inherited value of 'color' is currentColor, not the initial value." */
TEST(CssomComputed, AHighlightInheritsFromTheSameHighlightOfTheParentElementWhetherOrNotThePropertyIsInherited) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "#p::selection { color: red; background-color: yellow; text-decoration-line: underline; }"
       "#c::selection { color: inherit; }"
       "#d::selection { background-color: unset; }"
       "#e::selection { background-color: initial; color: lime; }"
       "#g::selection { color: currentcolor; }"
       "</style></head><body><div id=p><span id=c><b id=d><i id=e>x</i></b></span><s id=g>y</s></div></body></html>");
  auto read = [&](const char * element, const char * pseudo, const char * property) {
    Decl d = m.computed(element, pseudo);
    return get(d.d, property);
  };
  EXPECT_EQ(read("#p", "::selection", "background-color"), "rgb(255, 255, 0)");
  EXPECT_EQ(read("#c", "::selection", "background-color"), "rgb(255, 255, 0)") << "background-color is not inherited, and the highlight inherits it from the parent's highlight";
  EXPECT_EQ(read("#c", "::selection", "text-decoration-line"), "underline") << "so is every supported property";
  EXPECT_EQ(read("#c", "::selection", "color"), "rgb(255, 0, 0)") << "inherit, and no value at all, come to the same";
  EXPECT_EQ(read("#d", "::selection", "background-color"), "rgb(255, 255, 0)") << "unset is inheritance for every supported property";
  EXPECT_EQ(read("#d", "::selection", "color"), "rgb(255, 0, 0)") << "two levels down";
  EXPECT_EQ(read("#e", "::selection", "background-color"), "rgba(0, 0, 0, 0)") << "initial is a value the cascade gave";
  EXPECT_EQ(read("#e", "::selection", "color"), "rgb(0, 255, 0)");
  EXPECT_EQ(read("#e", "::selection", "text-decoration-line"), "underline");
  EXPECT_EQ(read("#g", "::selection", "color"), "rgb(255, 0, 0)") << "color: currentcolor is color: inherit, and what the highlight inherits is the parent's highlight";
  // a highlight of one kind does not inherit from another's
  EXPECT_EQ(read("#c", "::target-text", "background-color"), "rgba(0, 0, 0, 0)");
  // the element's own style is untouched
  EXPECT_EQ(read("#c", nullptr, "background-color"), "rgba(0, 0, 0, 0)");
}

/* "for highlight pseudo-elements originating from the root element the inherited value of 'color' is currentColor": the keyword, which each element that inherits it reads as its own color */
TEST(CssomComputed, TheColorOfARootHighlightIsCurrentColorAndStandsForTheColorOfTheElementThatInheritsIt) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "html { color: green; } #c { color: blue; } #d { color: red; } #f { color: black; }"
       "#d::selection { background-color: currentcolor; }"
       "</style></head><body><div id=c><span id=d>x<u id=f>z</u></span><span id=e>y</span></div></body></html>");
  auto read = [&](const char * element, const char * pseudo, const char * property) {
    Decl d = m.computed(element, pseudo);
    return get(d.d, property);
  };
  EXPECT_EQ(read("html", "::selection", "color"), "rgb(0, 128, 0)") << "the root's highlight: the root's color";
  EXPECT_EQ(read("#c", "::selection", "color"), "rgb(0, 0, 255)") << "not the root highlight's color as it was computed: the keyword, read as this element's";
  EXPECT_EQ(read("#d", "::selection", "color"), "rgb(255, 0, 0)");
  EXPECT_EQ(read("#d", "::selection", "background-color"), "rgb(255, 0, 0)") << "currentcolor in another property is the color of the highlight, which is its element's";
  EXPECT_EQ(read("#e", "::selection", "background-color"), "rgba(0, 0, 0, 0)") << "the parent's highlight has none";
  EXPECT_EQ(read("#f", "::selection", "background-color"), "rgb(0, 0, 0)") << "currentcolor is inherited as the keyword, so it is the color of the element that inherits it, not of the one that wrote it";
}

/* "highlight-cascade-parent-style-change": a highlight inherits from the current style of its parent's highlight, not from one read before */
TEST(CssomComputed, AHighlightInheritsFromTheCurrentStyleOfTheParentsHighlight) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "#parent { font-size: 10px; } #parent::selection { text-shadow: green 1em 0; }"
       "</style></head><body><div id=parent><span id=child>x</span></div></body></html>");
  auto shadow = [&](const char * element) {
    Decl d = m.computed(element, "::selection");
    return get(d.d, "text-shadow");
  };
  EXPECT_EQ(shadow("#parent"), "rgb(0, 128, 0) 10px 0px 0px");
  EXPECT_EQ(shadow("#child"), "rgb(0, 128, 0) 10px 0px 0px");
}

/* "highlight-pseudos-computed-search-text": `::search-text:not(:current)` matches the highlight that is not the current match, and `::search-text:current` does not (the pseudo-element asked for
   is not the focused match instance; the document says nothing of a current match) */
TEST(CssomComputed, TheSearchTextHighlightThatIsNotTheCurrentMatchTakesTheNotCurrentRule) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "#target::search-text { background-color: blue; color: lime; }"
       "#target::search-text:not(:current) { background-color: green; }"
       "#target::search-text:current { background-color: red; }"
       "</style></head><body><div id=target></div></body></html>");
  Decl d = m.computed("#target", "::search-text");
  EXPECT_EQ(get(d.d, "background-color"), "rgb(0, 128, 0)");
  EXPECT_EQ(get(d.d, "color"), "rgb(0, 255, 0)");
}

/* ---- the declaration ----------------------------------------------------------------------------------------------------------------------------------------- */

/* CSSOM, 7.2: "Return a live CSSStyleProperties object with the following properties: computed flag Set. readonly flag Set." and 6.6: "Setting the cssText attribute must run these
   steps: If the readonly flag is set, then throw a NoModificationAllowedError exception." and "Getting the cssText attribute must run these steps: If the computed flag is set, then return
   the empty string." */
TEST(CssomComputed, EveryMutatorIsNoModificationAndTheCssTextIsEmpty) {
  Cs m(kPage);
  Decl d = m.computed("#a");
  EXPECT_EQ(text_of(d.d), "");
  EXPECT_EQ(set(d.d, "color", "red").st, GTEXT_CSS_E_NO_MODIFICATION);
  EXPECT_EQ(set(d.d, "bogus", "red").st, GTEXT_CSS_E_NO_MODIFICATION);
  EXPECT_EQ(set(d.d, "color", "").st, GTEXT_CSS_E_NO_MODIFICATION);
  EXPECT_EQ(remove(d.d, "color").st, GTEXT_CSS_E_NO_MODIFICATION);
  EXPECT_EQ(set_text(d.d, "color: red").st, GTEXT_CSS_E_NO_MODIFICATION);
  GTEXT_CSSOM_Outcome o;
  EXPECT_EQ(gtext_cssom_decl_idl_set(d.d, "backgroundColor", 15, "red", 3, &o), GTEXT_CSS_E_NO_MODIFICATION);
  EXPECT_EQ(get(d.d, "color"), "rgb(10, 20, 30)") << "unchanged";
  EXPECT_EQ(priority(d.d, "color"), "") << "no declaration of a computed style is important";
  EXPECT_EQ(priority(d.d, "margin"), "");
}

TEST(CssomComputed, TheValuesFollowTheCascadeAndInheritance) {
  Cs m(kPage);
  EXPECT_EQ(m.value("#d", "color"), "rgb(10, 20, 30)");
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)") << "inherited from the div";
  EXPECT_EQ(m.value("#s1", "color"), "canvastext") << "the initial value is a system color; the environment of the default context has no color callback to resolve it, and nothing is guessed";
  EXPECT_EQ(m.value("#d", "margin-top"), "7px");
  EXPECT_EQ(m.value("#a", "margin-top"), "0px");
  EXPECT_EQ(m.value("#a", "COLOR"), "rgb(10, 20, 30)") << "a property name is ASCII case-insensitive";
  EXPECT_EQ(m.value("#a", "bogus"), "");
  EXPECT_EQ(m.value("#a", "--bogus"), "");
}

/* "getPropertyValue of a shorthand builds a declaration block of the resolved longhands": the shorthand is written from them. */
TEST(CssomComputed, AShorthandIsWrittenFromTheResolvedLonghands) {
  Cs m("<!DOCTYPE html><html><head><style>#a{margin:1px 2px 3px 4px;padding:5px} #b{margin:1px}</style></head><body><p id=a>x</p><p id=b>y</p></body></html>");
  EXPECT_EQ(m.value("#a", "margin"), "1px 2px 3px 4px");
  EXPECT_EQ(m.value("#b", "margin"), "1px");
  EXPECT_EQ(m.value("#a", "padding"), "5px");
  EXPECT_EQ(m.value("#a", "margin-block"), "1px 3px");
  Decl d = m.computed("#a");
  EXPECT_EQ(origin(d.d, "margin"), "absent-layout") << "margin is a used value, and no layout was given";
  EXPECT_EQ(origin(d.d, "color"), "computed");
  EXPECT_EQ(origin(d.d, "bogus"), "absent");
}

TEST(CssomComputed, ANewObjectEachCallAndTheSameValuesInEachWhileNothingMoves) {
  Cs m(kPage);
  Decl a = m.computed("#a");
  Decl b = m.computed("#a");
  EXPECT_NE(a.d, b.d) << "[NewObject]";
  EXPECT_EQ(names(a.d), names(b.d));
  EXPECT_EQ(get(a.d, "color"), get(b.d, "color"));
  const std::string once = dump(a.d);
  EXPECT_EQ(dump(a.d), once) << "two reads at one state are byte-identical";
}

/* ---- live ------------------------------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomComputed, ADeclarationIsLiveAfterAMutationOfTheDocument) {
  Cs m(kPage);
  Decl a = m.computed("#a");
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)");
  const uint64_t v0 = gtext_cssom_decl_version(a.d);
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  ASSERT_NE(sh, nullptr);
  EXPECT_EQ(insert_rule(sh, "#a{color:orange}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(get(a.d, "color"), "rgb(255, 165, 0)") << "the same object, after insertRule";
  EXPECT_GT(gtext_cssom_decl_version(a.d), v0);
  EXPECT_EQ(delete_rule(sh, 0), GTEXT_CSS_OK);
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)");
  gtext_cssom_sheet_release(sh);
}

TEST(CssomComputed, ADeclarationIsLiveAfterAChangeOfTheTreeThroughTheVersionHook) {
  Cs m(kPage);
  Decl a = m.computed("#a");
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)");
  set_attr(m.el("#a"), "class", "x");
  EXPECT_EQ(get(a.d, "color"), "rgb(0, 0, 255)");
  set_attr(m.el("#a"), "style", "color: green !important");
  EXPECT_EQ(get(a.d, "color"), "rgb(0, 128, 0)") << "the style attribute is the element interface's";
  set_attr(m.el("#d"), "style", "margin-top: 9px");
  Decl d = m.computed("#d");
  EXPECT_EQ(get(d.d, "margin-top"), "9px");
}

/* The adapter of the Markup tree keeps the classes of the element it was asked about last; an edit of that element's `class` under it must be seen by the next question (the matcher
   asks a sibling's class last when it tests `.sibling + #a`). */
TEST(CssomComputed, AClassSetOnTheElementTheMatcherAskedAboutLastIsSeen) {
  Cs m("<!DOCTYPE html><html><head><style>div{color:red} .sibling + #a{color:green} .on{color:blue}</style></head><body><div id=h></div><div id=a></div><div id=b></div></body></html>");
  Decl a = m.computed("#a");
  EXPECT_EQ(get(a.d, "color"), "rgb(255, 0, 0)");
  set_attr(m.el("#h"), "class", "sibling");
  EXPECT_EQ(get(a.d, "color"), "rgb(0, 128, 0)") << "the previous sibling was asked about last, with no class";
  set_attr(m.el("#h"), "class", "other");
  EXPECT_EQ(get(a.d, "color"), "rgb(255, 0, 0)");
  set_attr(m.el("#a"), "class", "on");
  EXPECT_EQ(get(a.d, "color"), "rgb(0, 0, 255)") << "and the element itself";
}

/* The cost: "n calls over a chain of depth d cost O(d + n) computes, not O(d x n), asserted by a counter." */
TEST(CssomComputed, TheChainIsComputedOnceAndTheNextReadsCostNothing) {
  std::string html = "<!DOCTYPE html><html><head><style>div{color:rgb(1,2,3)}</style></head><body>";
  const int depth = 60;
  for (int i = 0; i < depth; i++) {
    html += "<div id=d" + std::to_string(i) + ">";
  }
  html += "<p id=leaf>x</p>";
  for (int i = 0; i < depth; i++) {
    html += "</div>";
  }
  html += "</body></html>";
  Cs m(html);
  const uint64_t c0 = m.dc().computed_computes;
  EXPECT_EQ(m.value("#leaf", "color"), "rgb(1, 2, 3)");
  const uint64_t first = m.dc().computed_computes - c0;
  EXPECT_GE(first, static_cast<uint64_t>(depth)) << "the chain, html and body included";
  EXPECT_LE(first, static_cast<uint64_t>(depth) + 4);
  for (int i = 0; i < 50; i++) {
    EXPECT_EQ(m.value("#leaf", "color"), "rgb(1, 2, 3)");
  }
  EXPECT_EQ(m.dc().computed_computes - c0, first) << "n more reads: no more computes";
  // a neighbour in the same chain costs one
  const uint64_t before = m.dc().computed_computes;
  EXPECT_EQ(m.value("#d30", "color"), "rgb(1, 2, 3)");
  EXPECT_EQ(m.dc().computed_computes, before) << "an ancestor is in the cache already";
  // a mutation drops the cache: the chain is computed again, once
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("style"));
  EXPECT_EQ(insert_rule(sh, "p{margin-top:1px}", 0).st, GTEXT_CSS_OK);
  gtext_cssom_sheet_release(sh);
  const uint64_t b2 = m.dc().computed_computes;
  EXPECT_EQ(m.value("#leaf", "margin-top"), "1px");
  EXPECT_GE(m.dc().computed_computes - b2, static_cast<uint64_t>(depth));
  EXPECT_LE(m.dc().computed_computes - b2, static_cast<uint64_t>(depth) + 4);
}

/* "A config with neither the version hook nor `tree_changed` disables cross-call caching: each `getComputedStyle` read recomputes its chain (counted)". */
TEST(CssomComputed, WithNoHookAndNoNotificationEveryReadComputesItsChain) {
  Cs m(kPage, [](GTEXT_CSSOM_Config & c) {
    c.tree_version = nullptr;
    c.tree_version_user = nullptr;
  });
  const uint64_t c0 = m.dc().computed_computes;
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)");
  const uint64_t one = m.dc().computed_computes - c0;
  EXPECT_GE(one, 3u);
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)");
  EXPECT_EQ(m.dc().computed_computes - c0, 2 * one) << "nothing is cached across calls";
  // and each call of one object reads again
  Decl a = m.computed("#a");
  const uint64_t before = m.dc().computed_computes;
  (void)get(a.d, "color");
  (void)get(a.d, "width");
  EXPECT_EQ(m.dc().computed_computes - before, 2 * one);
}

TEST(CssomComputed, WithTheNotificationTheCacheIsKeptUntilTheTreeChanges) {
  Cs m(kPage, [](GTEXT_CSSOM_Config & c) {
    c.tree_version = nullptr;
    c.tree_version_user = nullptr;
    c.computed_trust_tree_changed = true;
  });
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)");
  const uint64_t c0 = m.dc().computed_computes;
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)");
  EXPECT_EQ(m.dc().computed_computes, c0);
  set_attr(m.el("#a"), "class", "x");
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)") << "the caller has not said: the cache answers";
  gtext_cssom_document_tree_changed(m.d());
  EXPECT_EQ(m.value("#a", "color"), "rgb(0, 0, 255)");
}

TEST(CssomComputed, TheCacheIsBoundedAndTheAnswerDoesNotDependOnIt) {
  std::string html = "<!DOCTYPE html><html><head><style>div{color:rgb(1,2,3)} #leaf{margin-top:3px}</style></head><body>";
  for (int i = 0; i < 40; i++) {
    html += "<div>";
  }
  html += "<p id=leaf>x</p>";
  for (int i = 0; i < 40; i++) {
    html += "</div>";
  }
  html += "</body></html>";
  Cs bounded(html, [](GTEXT_CSSOM_Config & c) { c.max_cached_styles = 5; });
  Cs unbounded(html);
  Decl a = bounded.computed("#leaf");
  Decl b = unbounded.computed("#leaf");
  EXPECT_EQ(names(a.d), names(b.d));
  for (const std::string & n : names(a.d)) {
    EXPECT_EQ(get(a.d, n), get(b.d, n)) << n;
  }
  EXPECT_LE(bounded.dc().cached_styles, 5u);
  EXPECT_GT(bounded.dc().computed_cache_evictions, 0u);
  EXPECT_EQ(unbounded.dc().computed_cache_evictions, 0u);
  EXPECT_GE(unbounded.dc().cached_styles, 40u);
}

/* ---- resolved values --------------------------------------------------------------------------------------------------------------------------------------- */

/* CSSOM, 9: "background-color, border-block-end-color, ... box-shadow, caret-color, color, outline-color: A resolved value special case property like color ... The resolved value is
   the used value." The used value of `currentcolor` is the element's color. */
TEST(CssomComputed, TheUsedValueOfCurrentcolorIsTheElementsColor) {
  Cs m(kPage);
  EXPECT_EQ(m.value("#a", "background-color"), "rgb(10, 20, 30)") << "p{background-color: currentcolor}";
  EXPECT_EQ(m.value("#a", "border-top-color"), "rgb(10, 20, 30)") << "the initial value of the border colors is currentcolor";
  EXPECT_EQ(m.value("#a", "outline-color"), "rgb(10, 20, 30)") << "the initial value in the pin is auto, whose used value is the current color: both browsers answer so";
  EXPECT_EQ(m.value("#a", "caret-color"), "rgb(10, 20, 30)");
  EXPECT_EQ(m.value("#a", "text-decoration-color"), "rgb(10, 20, 30)") << "Color 4, 15: \"The resolved value of a <color> is its used value\", in a property section 9 does not list too";
  Decl d = m.computed("#a");
  EXPECT_EQ(origin(d.d, "background-color"), "used");
  EXPECT_EQ(origin(d.d, "border-left-color"), "used");
  EXPECT_EQ(origin(d.d, "color"), "computed");
  EXPECT_EQ(origin(d.d, "text-decoration-color"), "used");
  EXPECT_EQ(origin(d.d, "display"), "computed");
}

/* Color 4, 15.1: "If the sRGB color was explicitly specified by the author as a named color, ... The computed and used value is the corresponding sRGB color"; 16.2: "the serialized
   computed value of transparent is the string rgba(0, 0, 0, 0)". Every <color> in a value is resolved, wherever the value is. */
TEST(CssomComputed, EveryColorInAValueIsItsUsedValue) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "#a{color:red;background-image:linear-gradient(red, transparent 50%, currentcolor);text-shadow:1px 2px blue, 3px 4px currentcolor;"
       "text-emphasis-color:navy;border-top-color:transparent;scrollbar-color:red blue;filter:drop-shadow(1px 1px purple)}"
       "</style></head><body><p id=a>x</p></body></html>");
  EXPECT_EQ(m.value("#a", "color"), "rgb(255, 0, 0)");
  EXPECT_EQ(m.value("#a", "background-image"), "linear-gradient(rgb(255, 0, 0), rgba(0, 0, 0, 0) 50%, rgb(255, 0, 0))");
  EXPECT_EQ(m.value("#a", "text-shadow"), "rgb(0, 0, 255) 1px 2px 0px, rgb(255, 0, 0) 3px 4px 0px");
  EXPECT_EQ(m.value("#a", "text-emphasis-color"), "rgb(0, 0, 128)");
  EXPECT_EQ(m.value("#a", "border-top-color"), "rgba(0, 0, 0, 0)");
  EXPECT_EQ(m.value("#a", "scrollbar-color"), "rgb(255, 0, 0) rgb(0, 0, 255)");
  EXPECT_EQ(m.value("#a", "filter"), "drop-shadow(rgb(128, 0, 128) 1px 1px 0px)");
  EXPECT_EQ(m.value("#a", "accent-color"), "auto") << "auto is not a color";
}

/* CSSOM, "serialize a CSS value": "If component values can be omitted or replaced with a shorter representation without changing the meaning of the value, omit/replace them." The computed
   value of text-shadow is four lengths (a spread of 0px when none is written) and that of text-emphasis-style a pair of fill and shape; their resolved values are the shortest forms,
   which is what both browsers write (a zero spread is left out and `filled`, which a lone shape means, is left out). The spread stays when it is not zero, `open` stays, and the
   blur stays (Backgrounds 3 has it before the spread, and both browsers keep it). The computed value itself is unchanged. */
TEST(CssomComputed, TheResolvedValueOfAShadowAndAnEmphasisStyleIsTheShortestForm) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "#a{text-shadow:red 1px 2px 3px 4px, blue 1px 2px 3px 0px inset, green 5px 6px;text-emphasis-style:filled triangle}"
       "#b{text-shadow:none;text-emphasis-style:open triangle}"
       "#c{text-emphasis-style:filled}"
       "#d{text-emphasis-style:open;text-shadow:red 1px 2px 0 0}"
       "#e{text-emphasis-style:'x'}"
       "#f{text-emphasis:filled dot red}"
       "</style></head><body><p id=a>x</p><p id=b>x</p><p id=c>x</p><p id=d>x</p><p id=e>x</p><p id=f>x</p></body></html>");
  EXPECT_EQ(m.value("#a", "text-shadow"), "rgb(255, 0, 0) 1px 2px 3px 4px, rgb(0, 0, 255) 1px 2px 3px inset, rgb(0, 128, 0) 5px 6px 0px");
  EXPECT_EQ(m.value("#a", "text-emphasis-style"), "triangle");
  EXPECT_EQ(m.value("#b", "text-shadow"), "none");
  EXPECT_EQ(m.value("#b", "text-emphasis-style"), "open triangle");
  EXPECT_EQ(m.value("#c", "text-emphasis-style"), "circle") << "a lone filled computes to circle in a horizontal mode";
  EXPECT_EQ(m.value("#d", "text-emphasis-style"), "open circle");
  EXPECT_EQ(m.value("#d", "text-shadow"), "rgb(255, 0, 0) 1px 2px 0px") << "an explicit zero spread is left out too";
  EXPECT_EQ(m.value("#e", "text-emphasis-style"), "\"x\"");
  EXPECT_EQ(m.value("#f", "text-emphasis-style"), "dot");
  EXPECT_EQ(m.value("#f", "text-emphasis"), "dot rgb(255, 0, 0)");
}

TEST(CssomComputed, ACurrentcolorInsideAColorMixAndABoxShadowIsReplacedAndTheMixComputed) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "#a{color:rgb(255,0,0);background-color:color-mix(in srgb, currentcolor, rgb(0,0,255));box-shadow:1px 2px 3px currentcolor}"
       "#b{color:rgb(0,0,255);box-shadow:1px 2px 3px red, 4px 5px currentcolor}</style></head><body><p id=a>x</p><p id=b>y</p></body></html>");
  EXPECT_EQ(m.value("#a", "background-color"), "color(srgb 0.5 0 0.5)") << "color-mix(in srgb) with currentcolor in its place";
  EXPECT_EQ(m.value("#a", "box-shadow-color"), "rgb(255, 0, 0)");
  EXPECT_EQ(m.value("#b", "box-shadow-color"), "rgb(255, 0, 0), rgb(0, 0, 255)");
  Decl d = m.computed("#a");
  EXPECT_EQ(origin(d.d, "background-color"), "used");
  EXPECT_EQ(origin(d.d, "box-shadow"), "used") << "a shorthand is used when a longhand is";
}

/* ---- the used values: the layout interface --------------------------------------------------------------------------------------------------------------- */

/* CSSOM, 9: "height ... A resolved value special case property like height defined in another specification: If the property applies to the element or pseudo-element and the resolved
   value of the display property is not none or contents, then the resolved value is the used value. Otherwise the resolved value is the computed value." */
TEST(CssomComputed, AUsedValueComesFromTheLayoutWhenThePropertyAppliesAndDisplayIsNotNone) {
  Layout lay;
  lay.used["width"] = "49.5px";
  lay.used["margin-top"] = "7px";
  lay.used["height"] = "n/a";   // "does not apply, use computed"
  Cs m("<!DOCTYPE html><html><head><style>p{width:50px;height:12px;margin-top:3px} .n{display:none} .c{display:contents}</style></head><body>"
       "<p id=a>x</p><p id=n class=n>y</p><p id=c class=c>z</p></body></html>",
      [&](GTEXT_CSSOM_Config & c) { lay.install(c); });
  Decl a = m.computed("#a");
  EXPECT_EQ(get(a.d, "width"), "49.5px");
  EXPECT_EQ(get(a.d, "margin-top"), "7px");
  EXPECT_EQ(get(a.d, "height"), "12px") << "the layout said it does not apply";
  EXPECT_EQ(origin(a.d, "width"), "used");
  EXPECT_EQ(origin(a.d, "height"), "computed");
  EXPECT_EQ(get(a.d, "min-width"), "0px") << "an auto minimum size of a block is zero (Sizing 3), by the rule of the library, not by layout";
  EXPECT_EQ(origin(a.d, "min-width"), "used");
  Decl n = m.computed("#n");
  EXPECT_EQ(get(n.d, "width"), "50px") << "display: none";
  EXPECT_EQ(origin(n.d, "width"), "computed");
  Decl c = m.computed("#c");
  EXPECT_EQ(get(c.d, "width"), "50px") << "display: contents";
  EXPECT_EQ(get(c.d, "margin-top"), "3px");
  for (const std::string & asked : lay.asked) {
    EXPECT_NE(asked, "min-width");
    EXPECT_NE(asked, "color");
  }
}

TEST(CssomComputed, WithoutALayoutTheComputedValueStandsInAndTheOriginSaysSo) {
  Cs m(kPage);
  EXPECT_EQ(m.value("#a", "width"), "50px");
  Decl a = m.computed("#a");
  EXPECT_EQ(origin(a.d, "width"), "absent-layout");
  EXPECT_EQ(origin(a.d, "line-height"), "absent-layout");
  EXPECT_EQ(origin(a.d, "margin-top"), "absent-layout");
  EXPECT_EQ(origin(a.d, "padding-left"), "absent-layout");
  EXPECT_EQ(origin(a.d, "top"), "computed") << "the element is not positioned: the computed value, and layout is not needed to know it";
  Layout lay;
  lay.has_used = false;
  Cs m2(kPage, [&](GTEXT_CSSOM_Config & c) { lay.install(c); });
  Decl b = m2.computed("#a");
  EXPECT_EQ(origin(b.d, "width"), "absent-layout") << "an interface with no used_value is no layout";
}

/* CSSOM, 9: "bottom, left, inset-block-end, ... top: If the property applies to a positioned element and the resolved value of the display property is not none or contents, and the
   property is not over-constrained, then the resolved value is the used value. Otherwise the resolved value is the computed value." */
TEST(CssomComputed, AnInsetIsUsedOnlyForAPositionedElement) {
  Layout lay;
  lay.used["top"] = "11px";
  lay.used["left"] = "n/a";   // over-constrained
  Cs m("<!DOCTYPE html><html><head><style>p{top:5px;left:6px} .rel{position:relative} .st{position:static}</style></head><body>"
       "<p id=a class=rel>x</p><p id=b class=st>y</p><p id=c>z</p></body></html>",
      [&](GTEXT_CSSOM_Config & c) { lay.install(c); });
  Decl a = m.computed("#a");
  EXPECT_EQ(get(a.d, "top"), "11px");
  EXPECT_EQ(get(a.d, "left"), "6px") << "over-constrained: the computed value";
  EXPECT_EQ(origin(a.d, "top"), "used");
  Decl b = m.computed("#b");
  EXPECT_EQ(get(b.d, "top"), "5px") << "not positioned";
  EXPECT_EQ(origin(b.d, "top"), "computed");
  EXPECT_EQ(get(m.computed("#c").d, "top"), "5px");
  Layout none;
  Cs m2("<!DOCTYPE html><html><head><style>p{top:5px;position:absolute}</style></head><body><p id=a>x</p></body></html>", [&](GTEXT_CSSOM_Config & c) { none.install(c); });
  Decl x = m2.computed("#a");
  EXPECT_EQ(origin(x.d, "top"), "computed") << "no answer from the layout stub for top: it does not apply";
  EXPECT_EQ(none.asked.size() > 0, true);
}

/* CSSOM, 9: "line-height: The resolved value is normal if the computed value is normal, or the used value otherwise." */
TEST(CssomComputed, LineHeightIsNormalOrTheUsedValue) {
  Layout lay;
  lay.used["line-height"] = "24px";
  Cs m(kPage, [&](GTEXT_CSSOM_Config & c) { lay.install(c); });
  EXPECT_EQ(m.value("#a", "line-height"), "24px");
  EXPECT_EQ(m.value("#s1", "line-height"), "normal");
  const size_t asked = lay.asked.size();
  Decl s = m.computed("#s1");
  EXPECT_EQ(origin(s.d, "line-height"), "computed");
  EXPECT_EQ(std::count(lay.asked.begin() + static_cast<long>(asked), lay.asked.end(), "line-height"), 0) << "normal is not asked of the layout";
}

/* CSSOM, 9: "line-height: The resolved value is normal if the computed value is normal, or the used value otherwise." The used value of a line-height that is an absolute length is that
   length (CSS Inline 3: "Computed value: for <length> the absolute length"), a fact that needs no box: it is answered without the layout, with or without one, and with a layout the
   layout is not asked. A number is the font size times it, a fact of the layout as before; a percentage is computed to a length, and is answered. */
TEST(CssomComputed, AnAbsoluteLineHeightIsItsOwnUsedValueWithoutALayout) {
  const char * page = "<!DOCTYPE html><html><head><style>"
                      "#a{line-height:35px;font:12px/20px serif} #b{line-height:0px} #c{line-height:3em;font-size:10px} #d{line-height:1.5} #e{line-height:150%}"
                      "</style></head><body><p id=a>x</p><p id=b>x</p><p id=c>x</p><p id=d>x</p><p id=e>x</p></body></html>";
  Cs bare(page);
  EXPECT_EQ(bare.value("#a", "line-height"), "20px");
  EXPECT_EQ(bare.value("#a", "font"), "12px / 20px serif");
  EXPECT_EQ(bare.value("#b", "line-height"), "0px");
  EXPECT_EQ(bare.value("#c", "line-height"), "30px");
  Decl a = bare.computed("#a");
  EXPECT_EQ(origin(a.d, "line-height"), "used") << "no layout was needed, none was missing";
  EXPECT_EQ(origin(a.d, "font-size"), "computed");
  EXPECT_EQ(origin(bare.computed("#d").d, "line-height"), "absent-layout") << "a number is the font size times it";
  EXPECT_EQ(bare.value("#e", "line-height"), "24px") << "a percentage is computed to the absolute length it is of the font size (16px)";
  EXPECT_EQ(origin(bare.computed("#e").d, "line-height"), "used");
  Layout lay;
  lay.used["line-height"] = "99px";
  Cs m(page, [&](GTEXT_CSSOM_Config & c) { lay.install(c); });
  EXPECT_EQ(m.value("#a", "line-height"), "20px") << "the layout is not asked for a length";
  EXPECT_EQ(m.value("#d", "line-height"), "99px") << "and is asked for a number";
}

/* "any other status fails the read" and "a callback that fails with GTEXT_CSS_E_UNSUPPORTED fails the read, naming the callback, never a guess" */
TEST(CssomComputed, ALayoutThatFailsFailsTheReadAndNamesItself) {
  Layout lay;
  lay.fail = GTEXT_CSS_E_UNSUPPORTED;
  Cs m(kPage, [&](GTEXT_CSSOM_Config & c) { lay.install(c); });
  Decl a = m.computed("#a");
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_decl_length(a.d, &n), GTEXT_CSS_E_UNSUPPORTED);
  GTEXT_CSSOM_Computed_Failure f;
  ASSERT_EQ(gtext_cssom_computed_failure(m.d(), &f), GTEXT_CSS_OK);
  EXPECT_EQ(f.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(f.callback, "used_value");
  EXPECT_NE(f.property, 0u);
  lay.fail = GTEXT_CSS_OK;
  EXPECT_GT(length(a.d), 400u) << "the declaration is as it was and reads again";
}

/* CSS Animations 2: "the resolved value of auto for animation-duration is 0s whenever its used value would also be 0s". */
TEST(CssomComputed, AnAutoDurationIsZeroSecondsWhenTheTimelineIsAuto) {
  Cs m("<!DOCTYPE html><html><head><style>#a{animation-duration:auto} #b{animation-duration:auto, 2s} #c{animation-duration:2s}</style></head><body>"
       "<p id=a>x</p><p id=b>y</p><p id=c>z</p></body></html>");
  EXPECT_EQ(m.value("#a", "animation-duration"), "0s");
  EXPECT_EQ(m.value("#b", "animation-duration"), "0s, 2s");
  EXPECT_EQ(m.value("#c", "animation-duration"), "2s");
  Decl a = m.computed("#a");
  EXPECT_EQ(origin(a.d, "animation-duration"), "used");
}

/* CSS Sizing 3: "For backwards-compatibility, the resolved value of this keyword is zero for boxes of all [CSS2] display types ... when aspect-ratio is auto. It also resolves to zero when
   no box is generated." */
TEST(CssomComputed, AnAutoMinimumSizeIsZeroExceptForTheItemsOfAFlexOrGridContainer) {
  Cs m("<!DOCTYPE html><html><head><style>.f{display:flex} .g{display:grid} .n{display:none} .r{aspect-ratio:2} .i{display:inline-block} .ab{position:absolute} div{min-width:auto;min-height:auto}</style></head><body>"
       "<div id=a>x</div><div id=f class=f><div id=fi>y</div><div id=fa class=ab>z</div><div id=fr class=r>q</div></div><div id=g class=g><div id=gi>z</div></div><div id=n class=n>q</div>"
       "<div id=r class=r>r</div><div id=i class=i>i</div><div id=k style='min-width:5px'>k</div></body></html>");
  EXPECT_EQ(m.value("#a", "min-width"), "0px");
  EXPECT_EQ(m.value("#f", "min-width"), "0px") << "a flex container that is not an item: both browsers answer 0px";
  EXPECT_EQ(m.value("#fi", "min-width"), "auto") << "a flex item";
  EXPECT_EQ(m.value("#fi", "min-height"), "auto");
  EXPECT_EQ(m.value("#fa", "min-width"), "0px") << "an absolutely positioned child is no flex item";
  EXPECT_EQ(m.value("#gi", "min-height"), "auto") << "a grid item";
  EXPECT_EQ(m.value("#g", "min-height"), "0px");
  EXPECT_EQ(m.value("#n", "min-width"), "0px") << "no box is generated";
  EXPECT_EQ(m.value("#r", "min-width"), "auto") << "aspect-ratio is not auto: the sentence does not apply (Chromium answers 0px, Firefox auto)";
  EXPECT_EQ(m.value("#i", "min-width"), "0px");
  EXPECT_EQ(m.value("#k", "min-width"), "5px");
  EXPECT_EQ(m.value("#a", "min-inline-size"), "0px");
  Decl fi = m.computed("#fi");
  EXPECT_EQ(origin(fi.d, "min-width"), "computed");
  Decl a = m.computed("#a");
  EXPECT_EQ(origin(a.d, "min-width"), "used");
}

/* CSS Text 3: "a computed letter-spacing of zero yields a resolved value ... of normal"; CSS Borders 4: "The resolved value for the border-width properties is the used value. If the
   border-style corresponding to a given border-width is none or hidden, then the used width is 0." */
TEST(CssomComputed, TheComputedValuesThatAreTheUsedValuesAlready) {
  Cs m("<!DOCTYPE html><html><head><style>#a{letter-spacing:0px;border-left-style:none;border-left-width:5px;border-top-style:solid;border-top-width:thick}</style></head>"
       "<body><p id=a>x</p></body></html>");
  EXPECT_EQ(m.value("#a", "letter-spacing"), "normal");
  EXPECT_EQ(m.value("#a", "border-left-width"), "0px");
  EXPECT_EQ(m.value("#a", "border-top-width"), "5px");
}

/* Backgrounds 3: the computed value of a border width is the snapped length; "the resolved value ... is the used value", and the used width is 0 under `none` and `hidden`. So the
   parent's width resolves to 0px and the child that inherits it has the length (the page border-width-computed.html of the web-platform-tests; both pinned browsers answer 5px). */
TEST(CssomComputed, ABorderWidthResolvesToZeroUnderNoneButIsInheritedAsTheLength) {
  Cs m("<!DOCTYPE html><html><head><style>#p{border-top:5px none;border-bottom:thick hidden}"
       "#q{border-block-start:4px none}"
       "#c{border-top-style:solid;border-top-width:inherit;border-bottom-style:solid;border-bottom-width:inherit;border-inline-start:solid;border-inline-start-width:inherit}"
       "#d{border-top-width:inherit;border-left:3px solid}</style></head><body><div id=p><div id=c><div id=d>x</div></div></div><div id=q>y</div></body></html>");
  EXPECT_EQ(m.value("#p", "border-top-width"), "0px");
  EXPECT_EQ(m.value("#p", "border-bottom-width"), "0px");
  EXPECT_EQ(m.value("#q", "border-block-start-width"), "0px");
  EXPECT_EQ(m.value("#c", "border-top-width"), "5px");
  EXPECT_EQ(m.value("#c", "border-bottom-width"), "5px");
  // a child whose own style is none resolves what it inherited to zero
  EXPECT_EQ(m.value("#d", "border-top-width"), "0px");
  Decl p = m.computed("#p");
  EXPECT_EQ(origin(p.d, "border-top-width"), "used");
  Decl c = m.computed("#c");
  EXPECT_EQ(origin(c.d, "border-top-width"), "computed");
}

/* Nesting 1: "The specificity of the nesting selector is equal to the largest specificity among the complex selectors in the parent style rule's selector list ... or zero if no such
   selector list exists." A top-level `&` matches as `:scope` does and weighs nothing, so the later of two top-level rules wins (the page top-level-parent-pseudo-specificity.html). */
TEST(CssomComputed, ATopLevelNestingSelectorHasNoSpecificity) {
  Cs m("<!DOCTYPE html><html><head><style>& { color: red; } :where(&) { color: green; }</style></head><body><p id=a>x</p></body></html>");
  EXPECT_EQ(m.value("html", "color"), "rgb(0, 128, 0)");
  Cs n("<!DOCTYPE html><html><head><style>:where(&) { color: green; } & { color: red; }</style></head><body><p id=a>x</p></body></html>");
  EXPECT_EQ(n.value("html", "color"), "rgb(255, 0, 0)") << "the later of two rules of no specificity wins";
  Cs o("<!DOCTYPE html><html><head><style>& { color: red; } html { color: green; }</style></head><body><p id=a>x</p></body></html>");
  EXPECT_EQ(o.value("html", "color"), "rgb(0, 128, 0)") << "and any selector outweighs it";
}

/* ---- lifetime -------------------------------------------------------------------------------------------------------------------------------------------------- */

/* CSS Transforms 2: "When the computed value is a <transform-list>, the resolved value is one <matrix()> function or one <matrix3d()> function": the library does not make the matrix, the
   layout does (a percentage in a translation needs the reference box); CSS Grid 2: a grid container's track lists are the used value, "Otherwise ... the resolved value is simply the
   computed value". */
TEST(CssomComputed, TheTransformAndTheTrackListsAreTheLayoutsAndTheComputedValueStandsInWithoutOne) {
  const char * page = "<!DOCTYPE html><html><head><style>p{transform:rotate(45deg);grid-template-columns:10px 1fr;transform-origin:10% 20%}</style></head><body><p id=a>x</p></body></html>";
  Cs bare(page);
  Decl none = bare.computed("#a");
  EXPECT_EQ(get(none.d, "transform"), "rotate(45deg)");
  EXPECT_EQ(origin(none.d, "transform"), "absent-layout");
  EXPECT_EQ(origin(none.d, "grid-template-columns"), "absent-layout");
  EXPECT_EQ(origin(none.d, "transform-origin"), "absent-layout");
  Layout lay;
  lay.used["transform"] = "matrix(0.707107, 0.707107, -0.707107, 0.707107, 0, 0)";
  lay.used["grid-template-columns"] = "10px 90px";
  lay.used["transform-origin"] = "n/a";
  Cs m(page, [&](GTEXT_CSSOM_Config & c) { lay.install(c); });
  Decl d = m.computed("#a");
  EXPECT_EQ(get(d.d, "transform"), "matrix(0.707107, 0.707107, -0.707107, 0.707107, 0, 0)");
  EXPECT_EQ(get(d.d, "grid-template-columns"), "10px 90px");
  EXPECT_EQ(get(d.d, "transform-origin"), "10% 20% 0px") << "does not apply: the computed value";
  EXPECT_EQ(origin(d.d, "transform"), "used");
  EXPECT_EQ(origin(d.d, "transform-origin"), "computed");
}

/* CSSOM 7.2: "elt is connected": an element that is not in the document the match options name (its chain ends at another top) has the empty declaration. */
TEST(CssomComputed, ALooseElementHasTheEmptyDeclaration) {
  Cs m(kPage);
  GTEXT_Markup_Node * loose = make(m.markup(), "p");
  GTEXT_CSSOM_Decl * d = nullptr;
  ASSERT_EQ(gtext_cssom_computed_style(m.d(), gtext_css_markup_handle(loose), nullptr, 0, &d), GTEXT_CSS_OK);
  Decl hold(d);
  EXPECT_EQ(length(d), 0u);
  bool empty = false;
  EXPECT_EQ(gtext_cssom_computed_is_empty(d, &empty), GTEXT_CSS_OK);
  EXPECT_TRUE(empty);
  append(m.el("#d"), loose);   // connected now: the same object has values
  EXPECT_GT(length(d), 400u);
  detach(loose);
  gtext_markup_node_free(loose);
}

TEST(CssomComputed, AFreedElementsDeclarationIsFrozenAndItsCacheEntryGoes) {
  Cs m(kPage);
  Decl a = m.computed("#a");
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)");
  const void * handle = m.handle("#a");
  gtext_cssom_document_element_released(m.d(), handle);
  GTEXT_Markup_Node * node = m.el("#a");
  set_attr(node, "class", "x");   // a change the frozen declaration must not see
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)") << "frozen as it was";
  Decl again = m.computed("#a");
  EXPECT_EQ(get(again.d, "color"), "rgb(0, 0, 255)") << "a declaration made afterwards is of the element as it is";
}

/* "gtext_cssom_document_element_released ... drops that element's inline-style entry and cached styles, so a reused address never answers for the old element." */
TEST(CssomComputed, AReleasedHandleIsComputedAgainEvenWhenNothingElseMoved) {
  Cs m(kPage);
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)");
  const uint64_t before = m.dc().computed_computes;
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)");
  EXPECT_EQ(m.dc().computed_computes, before) << "the cache answers";
  gtext_cssom_document_element_released(m.d(), m.handle("#a"));
  EXPECT_EQ(m.value("#a", "color"), "rgb(10, 20, 30)");
  EXPECT_EQ(m.dc().computed_computes, before + 1) << "the entry of the released handle went: the element is computed again, and only it";
}

/* A callback of the caller's layout is a callback: the mutation guard is held while it runs. */
TEST(CssomComputed, AMutationFromTheLayoutCallbackIsRefused) {
  struct Probe {
    GTEXT_CSSOM_Document * doc = nullptr;
    GTEXT_CSS_Status seen = GTEXT_CSS_OK;
    static GTEXT_CSS_Status used(void * user, const void *, const GTEXT_CSS_Selector_List *, GTEXT_CSS_Property_Id, bool * applies, GTEXT_CSS_Value_Set **) {
      Probe * self = static_cast<Probe *>(user);
      GTEXT_CSSOM_Decl * free_standing = nullptr;
      if (gtext_cssom_decl_new(self->doc, &free_standing) == GTEXT_CSS_OK) {
        GTEXT_CSSOM_Outcome o;
        self->seen = gtext_cssom_decl_set_property(free_standing, "color", 5, "red", 3, "", 0, &o);
        gtext_cssom_decl_release(free_standing);
      }
      *applies = false;
      return GTEXT_CSS_OK;
    }
  } probe;
  GTEXT_CSSOM_Layout_Interface iface{};
  iface.size = sizeof iface;
  iface.user = &probe;
  iface.used_value = &Probe::used;
  Cs m(kPage, [&](GTEXT_CSSOM_Config & c) { c.layout = &iface; });
  probe.doc = m.d();
  Decl a = m.computed("#a");
  EXPECT_GT(length(a.d), 400u);
  EXPECT_EQ(probe.seen, GTEXT_CSS_E_REENTRANT);
}

/* The document is the caller's to release or to change from inside the read of a computed style: from a callback of the layout the read in flight must still finish on
   what it holds, whatever the callback did to the document. */
namespace {

struct Reenter {
  std::function<void()> act;
  int calls = 0;
  static GTEXT_CSS_Status used(void * user, const void *, const GTEXT_CSS_Selector_List *, GTEXT_CSS_Property_Id, bool * applies, GTEXT_CSS_Value_Set **) {
    Reenter * self = static_cast<Reenter *>(user);
    if (self->calls++ == 0 && self->act) {
      self->act();
    }
    *applies = false;
    return GTEXT_CSS_OK;
  }
};

}  // namespace

TEST(CssomComputed, TheDocumentReleasedFromTheLayoutCallbackDoesNotEndTheReadInFlight) {
  Reenter r;
  GTEXT_CSSOM_Layout_Interface iface{};
  iface.size = sizeof iface;
  iface.user = &r;
  iface.used_value = &Reenter::used;
  UnmapAllocator ua;
  Cs m(kPage, [&](GTEXT_CSSOM_Config & c) { c.layout = &iface; }, &ua.alloc);
  r.act = [&] { m.doc->release(); };
  Decl a = m.computed("#a");
  EXPECT_GT(length(a.d), 400u) << "the read finished";
  EXPECT_EQ(r.calls >= 1, true);
}

/* gtext_cssom_document_element_released() is the embedder's call, and an embedder frees an element from wherever it likes, a callback of a read included. The read in
   flight finishes on what it holds, and the release takes effect when the outermost read ends: the record table the read walks is not edited under it. */
namespace {

const char * const kAll[] = {"#a", "#b", "#d", "#s1", "html", "body"};

}  // namespace

TEST(CssomComputed, ReleasingEveryElementFromTheLayoutCallbackOfARead) {
  Reenter r;
  GTEXT_CSSOM_Layout_Interface iface{};
  iface.size = sizeof iface;
  iface.user = &r;
  iface.used_value = &Reenter::used;
  UnmapAllocator ua;
  Cs m(kPage, [&](GTEXT_CSSOM_Config & c) { c.layout = &iface; }, &ua.alloc);
  Decl a = m.computed("#a");
  Decl b = m.computed("#b");
  EXPECT_EQ(get(b.d, "color"), "rgb(10, 20, 30)");   // the style of #d and its ancestors are cached
  r.calls = 0;
  r.act = [&] {
    for (const char * s : kAll) {
      gtext_cssom_document_element_released(m.d(), m.handle(s));
    }
  };
  EXPECT_GT(length(a.d), 400u) << "the read finished";
  EXPECT_GE(r.calls, 1) << "the callback ran during the read";
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)") << "frozen as the read left it";
  const uint64_t before = m.dc().computed_computes;
  Decl again = m.computed("#a");
  EXPECT_EQ(get(again.d, "color"), "rgb(10, 20, 30)");
  EXPECT_GT(m.dc().computed_computes, before) << "the entries of the released handles went when the read ended";
}

TEST(CssomComputed, ReleasingTheElementReadFromTheElementStateCallback) {
  struct S {
    std::function<void()> act;
    int calls = 0;
    static uint32_t state(void * user, const void *) {
      S * self = static_cast<S *>(user);
      if (self->calls++ == 0 && self->act) {
        self->act();
      }
      return GTEXT_CSSOM_ELEMENT_STATE_ALL;
    }
  } s;
  UnmapAllocator ua;
  Cs m(kPage, [&](GTEXT_CSSOM_Config & c) {
    c.element_state = &S::state;
    c.element_state_user = &s;
  }, &ua.alloc);
  Decl a = m.computed("#a");
  s.calls = 0;
  s.act = [&] {
    for (const char * sel : kAll) {
      gtext_cssom_document_element_released(m.d(), m.handle(sel));
    }
  };
  EXPECT_GT(length(a.d), 400u);
  EXPECT_GE(s.calls, 1) << "the callback ran during the read";
  const uint64_t before = m.dc().computed_computes;
  Decl again = m.computed("#a");
  EXPECT_EQ(get(again.d, "color"), "rgb(10, 20, 30)");
  EXPECT_GT(m.dc().computed_computes, before) << "a style the read made after the release was not kept for the released handle";
}

TEST(CssomComputed, ReplacingTheComputeContextFromTheLayoutCallbackIsRefusedWhileTheReadRuns) {
  Reenter r;
  GTEXT_CSSOM_Layout_Interface iface{};
  iface.size = sizeof iface;
  iface.user = &r;
  iface.used_value = &Reenter::used;
  UnmapAllocator ua;
  Cs m(kPage, [&](GTEXT_CSSOM_Config & c) { c.layout = &iface; }, &ua.alloc);
  GTEXT_CSS_Status seen = GTEXT_CSS_OK;
  r.act = [&] { seen = gtext_cssom_document_set_compute_context(m.d(), nullptr); };
  Decl a = m.computed("#a");
  EXPECT_GT(length(a.d), 400u);
  EXPECT_EQ(seen, GTEXT_CSS_E_REENTRANT);
}

TEST(CssomComputed, ADeclarationOutlivesTheDocumentHandleAndKeepsWhatItHad) {
  GTEXT_CSSOM_Decl * held = nullptr;
  {
    Cs m(kPage);
    Decl a = m.computed("#a");
    EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)");
    held = gtext_cssom_decl_retain(a.d);
    m.doc->release();
  }
  EXPECT_EQ(get(held, "color"), "rgb(10, 20, 30)");
  EXPECT_GT(length(held), 400u);
  EXPECT_EQ(set(held, "color", "red").st, GTEXT_CSS_E_NO_MODIFICATION);
  gtext_cssom_decl_release(held);
}

TEST(CssomComputed, AnUnreadDeclarationOutlivesTheDocumentHandleAndIsEmpty) {
  GTEXT_CSSOM_Decl * held = nullptr;
  {
    Cs m(kPage);
    Decl a = m.computed("#a");
    held = gtext_cssom_decl_retain(a.d);
    m.doc->release();
  }
  EXPECT_EQ(length(held), 0u);
  gtext_cssom_decl_release(held);
}

TEST(CssomComputed, ReplacingTheComputeContextMakesEveryDeclarationReadAgain) {
  Cs m(kPage);
  Decl a = m.computed("#a");
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)");
  const uint64_t c0 = m.dc().computed_computes;
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = gtext_css_markup_interface(m.page->adapter);
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  ASSERT_EQ(gtext_css_compute_context_new(R(), nullptr, &co, &ctx, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_document_set_compute_context(m.d(), ctx), GTEXT_CSS_OK);
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)");
  EXPECT_GT(m.dc().computed_computes, c0) << "the cache was dropped";
  EXPECT_EQ(gtext_cssom_document_set_compute_context(m.d(), nullptr), GTEXT_CSS_OK);
  gtext_css_compute_context_free(ctx);   // nothing the document holds refers to it
  EXPECT_EQ(get(a.d, "color"), "rgb(10, 20, 30)");
}

TEST(CssomComputed, TheArgumentsAreChecked) {
  Cs m(kPage);
  GTEXT_CSSOM_Decl * d = nullptr;
  EXPECT_EQ(gtext_cssom_computed_style(nullptr, m.handle("#a"), nullptr, 0, &d), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_computed_style(m.d(), nullptr, nullptr, 0, &d), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_computed_style(m.d(), m.handle("#a"), nullptr, 3, &d), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_computed_style(m.d(), m.handle("#a"), nullptr, 0, nullptr), GTEXT_CSS_E_INVALID);
  Doc bare([](GTEXT_CSSOM_Config &) {});
  EXPECT_EQ(gtext_cssom_computed_style(bare.d, m.handle("#a"), nullptr, 0, &d), GTEXT_CSS_E_UNSUPPORTED) << "no element interface";
  Decl free_standing(*m.doc);
  bool empty = false;
  EXPECT_EQ(gtext_cssom_computed_is_empty(free_standing.d, &empty), GTEXT_CSS_E_INVALID);
  GTEXT_CSSOM_Origin o;
  bool found;
  EXPECT_EQ(gtext_cssom_computed_origin(free_standing.d, "color", 5, &o, &found), GTEXT_CSS_E_INVALID);
  GTEXT_CSSOM_Computed_Failure f;
  EXPECT_EQ(gtext_cssom_computed_failure(nullptr, &f), GTEXT_CSS_E_INVALID);
}

/* A context the document makes answers `if(media())` and `if(supports())` with the conditions object the document evaluates with (Values 5, 8.3: "media( <media-feature> | <media-condition> )"),
   and follows an environment change that replaces the object: before, such an `if()` was GTEXT_CSS_E_UNSUPPORTED naming `condition`. */
TEST(CssomComputed, AnIfWithAMediaOrSupportsTestIsAnsweredFromTheDocumentsConditions) {
  Cs m("<!DOCTYPE html><html><head><style>"
       "#a{--m: if(media(width >= 1000px): wide; else: narrow); --h: if(media((height: 800px)): tall; else: short); --s: if(supports(display: grid): grid; else: nogrid)}"
       "</style></head><body><p id=a>x</p></body></html>");
  EXPECT_EQ(m.value("#a", "--m"), "wide") << "the default environment is 1280 by 800; a feature without its parentheses is a feature";
  EXPECT_EQ(m.value("#a", "--h"), "tall");
  EXPECT_EQ(m.value("#a", "--s"), "grid");
  GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(nullptr);
  env.viewport.width = 500;
  env.viewport.height = 500;
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.media = &env;
  GTEXT_CSS_Conditions * narrow = nullptr;
  ASSERT_EQ(gtext_css_conditions_new(R(), &co, &narrow), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_set_conditions(m.d(), narrow, nullptr), GTEXT_CSS_OK);
  gtext_css_conditions_free(narrow);
  EXPECT_EQ(m.value("#a", "--m"), "narrow") << "the replaced conditions object is the one asked";
  EXPECT_EQ(m.value("#a", "--h"), "short");
}

/* gtext_cssom_document_container_interface(): the styles the document has computed are what a query container is read from, so a style query of a `@container` rule is answered
   by the document that computes the style it is asked of. Without it the read is GTEXT_CSS_E_UNSUPPORTED naming `container.computed_style`. */
TEST(CssomComputed, TheContainerInterfaceOfADocumentAnswersAStyleQueryFromTheStylesItComputes) {
  const char * page = "<!DOCTYPE html><html><head><style>"
                      ".c{--k: 1} @container style(--k: 1) { #a{--hit: yes} } @container style(--k: 2) { #a{--miss: yes} }"
                      "</style></head><body><div class=c><p id=a>x</p></div></body></html>";
  Cs m(page);
  EXPECT_EQ(m.value("#a", "--hit"), "<not supported>");
  GTEXT_CSSOM_Computed_Failure f;
  ASSERT_EQ(gtext_cssom_computed_failure(m.d(), &f), GTEXT_CSS_OK);
  EXPECT_STREQ(f.callback, "container.computed_style");
  GTEXT_CSS_Container_Interface ci;
  ASSERT_EQ(gtext_cssom_document_container_interface(m.d(), &ci), GTEXT_CSS_OK);
  EXPECT_EQ(ci.size, sizeof ci);
  EXPECT_EQ(ci.user, m.d());
  ASSERT_NE(ci.computed_style, nullptr);
  EXPECT_EQ(ci.content_box, nullptr) << "a container's box is the caller's";
  EXPECT_EQ(ci.computed_style(ci.user, m.handle("#a")), nullptr) << "nothing is computed yet";
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.element_interface = gtext_css_markup_interface(m.page->adapter);
  co.container = &ci;
  GTEXT_CSS_Conditions * c = nullptr;
  ASSERT_EQ(gtext_css_conditions_new(R(), &co, &c), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_set_conditions(m.d(), c, nullptr), GTEXT_CSS_OK);
  gtext_css_conditions_free(c);
  EXPECT_EQ(m.value("#a", "--hit"), "yes");
  EXPECT_EQ(m.value("#a", "--miss"), "") << "the container's --k is 1, not 2";
  EXPECT_NE(ci.computed_style(ci.user, m.handle(".c")), nullptr) << "the container's style was computed on the way and is what the query read";
  GTEXT_CSSOM_Decl * none = nullptr;
  EXPECT_EQ(gtext_cssom_document_container_interface(nullptr, &ci), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_document_container_interface(m.d(), nullptr), GTEXT_CSS_E_INVALID);
  (void)none;
}

/* Backgrounds 4, 2.7 (the computed value of `background-position` is "a list, each item a pair of offsets (horizontal and vertical) from the top left origin"): the shorthand of a computed
   declaration is that pair, not the origin keywords of the longhands written as a shorthand (both pinned browsers: `5px 9px`, `0% 0%`; web-platform-tests css-values/calc-background-position-002.html). */
TEST(CssomComputed, TheComputedBackgroundPositionIsAPairOfOffsetsFromTheTopLeft) {
  Cs m("<!DOCTYPE html><html><body><div id=a style=\"background-position: 5px 9px\"></div><div id=b></div><div id=c style=\"background-position: right 10px bottom 3px\"></div>"
       "<div id=d style=\"background-position: 5px 9px, center\"></div><div id=e style=\"background-position: calc(10px + 1px) top\"></div></body></html>");
  EXPECT_EQ(m.value("#a", "background-position"), "5px 9px");
  EXPECT_EQ(m.value("#b", "background-position"), "0% 0%") << "the initial value";
  EXPECT_EQ(m.value("#c", "background-position"), "calc(100% - 10px) calc(100% - 3px)");
  EXPECT_EQ(m.value("#d", "background-position"), "5px 9px, 50% 50%");
  EXPECT_EQ(m.value("#e", "background-position"), "11px 0%");
  EXPECT_EQ(m.value("#a", "background-position-x"), "left 5px") << "the longhands keep the origin keyword";
  EXPECT_EQ(m.value("#a", "background-position-y"), "top 9px");
}

}  // namespace
