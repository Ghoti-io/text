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
 * The `all` shorthand in a CSSStyleDeclaration (CSS Cascade 5, "all"; CSSOM): it is written as its longhands, every property but `direction`, `unicode-bidi` and the custom
 * properties, so that a later declaration of a property overrides it, `removeProperty('all')` removes what it set, and `cssText` writes `all: x` only when nothing else is left of
 * it. The cases are those of web-platform-tests' cssom/cssstyledeclaration-all-shorthand.html, -csstext-all-shorthand.html and -removeProperty-all.html, and the places where
 * Chromium 156 and Firefox 158 agree with them.
 */

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

/// Every property `all` sets: the properties that are neither shorthands nor aliases nor custom, but `direction` and `unicode-bidi`.
std::vector<std::string> all_longhands() {
  std::vector<std::string> out;
  const size_t n = gtext_css_registry_property_count(R());
  for (size_t id = 1; id <= n; id++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(id), &info)) {
      continue;
    }
    if (info.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS | GTEXT_CSS_PROP_CUSTOM)) {
      continue;
    }
    const std::string name(info.name, info.name_length);
    if (name == "direction" || name == "unicode-bidi") {
      continue;
    }
    out.push_back(name);
  }
  return out;
}

/// A free declaration holding `text` as its cssText.
struct Style {
  Doc doc;
  Decl d;
  Style() : d(doc) {}
  explicit Style(const std::string & text) : d(doc) {
    EXPECT_EQ(set_text(d.d, text).st, GTEXT_CSS_OK) << text;
  }
};

}  // namespace

TEST(CssomAll, GetPropertyValueOfAllIsTheKeywordOrNothing) {
  {
    Style s("width: 50px");
    EXPECT_EQ(get(s.d.d, "all"), "") << "no `all`";
  }
  {
    Style s("all: revert");
    EXPECT_EQ(get(s.d.d, "all"), "revert");
    EXPECT_EQ(get(s.d.d, "width"), "revert");
    EXPECT_EQ(get(s.d.d, "color"), "revert");
    EXPECT_EQ(get(s.d.d, "direction"), "") << "`all` does not reset direction";
    EXPECT_EQ(get(s.d.d, "unicode-bidi"), "");
  }
  {
    Style s("all: revert; width: 50px");
    EXPECT_EQ(get(s.d.d, "all"), "") << "a property declared after it is not what it said";
    EXPECT_EQ(get(s.d.d, "width"), "50px");
    EXPECT_EQ(get(s.d.d, "color"), "revert");
  }
  {
    Style s("width: 50px; all: revert");
    EXPECT_EQ(get(s.d.d, "width"), "revert") << "`all` overrides the declaration before it";
    EXPECT_EQ(get(s.d.d, "all"), "revert");
  }
  {
    Style s("all: revert; width: 50px; all: unset");
    EXPECT_EQ(get(s.d.d, "width"), "unset");
    EXPECT_EQ(get(s.d.d, "all"), "unset");
  }
  {
    Style s("width: 50px; all: revert; width: 100px");
    EXPECT_EQ(get(s.d.d, "width"), "100px");
    EXPECT_EQ(get(s.d.d, "all"), "");
  }
}

TEST(CssomAll, TheSameInARulesStyle) {
  Doc doc;
  {
    Sheet s(doc, ".foo { all: revert; width: 50px; }");
    Rule r = path(s, {0});
    Decl d = style_of(r.r);
    EXPECT_EQ(get(d.d, "width"), "50px");
    EXPECT_EQ(get(d.d, "all"), "");
    EXPECT_EQ(get(d.d, "color"), "revert");
  }
  {
    Sheet s(doc, ".foo { width: 50px; all: revert; }");
    Rule r = path(s, {0});
    Decl d = style_of(r.r);
    EXPECT_EQ(get(d.d, "width"), "revert");
    EXPECT_EQ(get(d.d, "all"), "revert");
  }
  {
    Sheet s(doc, ".foo { all: revert; width: 50px; all: unset; }");
    Rule r = path(s, {0});
    Decl d = style_of(r.r);
    EXPECT_EQ(get(d.d, "width"), "unset");
    EXPECT_EQ(get(d.d, "all"), "unset");
    EXPECT_EQ(css(r.r), ".foo { all: unset; }");
  }
  {
    Sheet s(doc, ".foo { width: 50px; all: revert; width: 100px; }");
    Rule r = path(s, {0});
    Decl d = style_of(r.r);
    EXPECT_EQ(get(d.d, "width"), "100px");
    EXPECT_EQ(get(d.d, "all"), "");
  }
}

TEST(CssomAll, SetPropertyOverridesAndIsOverridden) {
  {
    Style s;
    EXPECT_EQ(set(s.d.d, "all", "revert").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(get(s.d.d, "width"), "revert");
    EXPECT_EQ(get(s.d.d, "color"), "revert");
  }
  {
    Style s("all: revert");
    EXPECT_EQ(set(s.d.d, "width", "50px").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(get(s.d.d, "width"), "50px");
    EXPECT_EQ(get(s.d.d, "all"), "");
  }
  {
    Style s("all: revert; width: 50px");
    EXPECT_EQ(set(s.d.d, "all", "unset").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(get(s.d.d, "width"), "unset");
    EXPECT_EQ(get(s.d.d, "all"), "unset");
  }
  {
    Style s("width: 50px; all: revert");
    EXPECT_EQ(set(s.d.d, "width", "100px").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(get(s.d.d, "width"), "100px");
    EXPECT_EQ(get(s.d.d, "all"), "");
  }
  {
    Style s("all: revert");
    EXPECT_EQ(set(s.d.d, "all", "revert").o.kind, GTEXT_CSSOM_UNCHANGED) << "nothing is different";
    EXPECT_EQ(set(s.d.d, "all", "bogus").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << "all takes a CSS-wide keyword only";
    EXPECT_EQ(set(s.d.d, "all", "10px").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
    EXPECT_EQ(get(s.d.d, "all"), "revert");
  }
}

TEST(CssomAll, RemovePropertyRemovesWhatAllSet) {
  {
    Style s("width: 50px; color: green; direction: rtl");
    EXPECT_EQ(remove(s.d.d, "all").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(get(s.d.d, "width"), "");
    EXPECT_EQ(get(s.d.d, "color"), "");
    EXPECT_EQ(get(s.d.d, "direction"), "rtl") << "direction is not a longhand of all";
  }
  {
    Style s("all: revert");
    EXPECT_EQ(remove(s.d.d, "all").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(get(s.d.d, "all"), "");
    EXPECT_EQ(length(s.d.d), 0u);
  }
  {
    // removeProperty-all.html
    Style s;
    EXPECT_EQ(set(s.d.d, "width", "40px").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(length(s.d.d), 1u);
    EXPECT_EQ(remove(s.d.d, "all").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(length(s.d.d), 0u) << "all is a shorthand of all properties, so it removes the property";
  }
  {
    Style s("width: 40px; all: revert");
    remove(s.d.d, "all");
    EXPECT_EQ(get(s.d.d, "all"), "");
    EXPECT_EQ(length(s.d.d), 0u);
  }
  {
    Style s("--a: 1; all: revert; direction: rtl");
    remove(s.d.d, "all");
    EXPECT_EQ(text_of(s.d.d), "--a: 1; direction: rtl;") << "custom properties and direction stay";
  }
}

TEST(CssomAll, ShorthandsReadTheKeywordOfAllOrTheLonghandsAfterIt) {
  {
    Style s("all: revert; margin: initial");
    EXPECT_EQ(get(s.d.d, "margin"), "initial");
    EXPECT_EQ(get(s.d.d, "all"), "");
  }
  {
    Style s("margin: initial; all: revert");
    EXPECT_EQ(get(s.d.d, "margin"), "revert");
    EXPECT_EQ(get(s.d.d, "all"), "revert");
  }
  {
    Style s("all: revert");
    EXPECT_EQ(set(s.d.d, "margin", "initial").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(get(s.d.d, "margin"), "initial");
    EXPECT_EQ(get(s.d.d, "all"), "");
  }
  {
    Style s("margin: initial");
    EXPECT_EQ(set(s.d.d, "all", "revert").o.kind, GTEXT_CSSOM_CHANGED);
    EXPECT_EQ(get(s.d.d, "margin"), "revert");
    EXPECT_EQ(get(s.d.d, "all"), "revert");
  }
  {
    Style s("margin-top: 10px; all: revert");
    EXPECT_EQ(get(s.d.d, "margin"), "revert");
    EXPECT_EQ(get(s.d.d, "all"), "revert");
    EXPECT_EQ(get(s.d.d, "font"), "revert");
  }
  {
    Style s("margin-top:10px; margin-right:20px; all: revert; margin-bottom:30px; margin-left:40px");
    EXPECT_EQ(get(s.d.d, "margin"), "") << "some of its longhands are all's, some are not";
    EXPECT_EQ(get(s.d.d, "all"), "");
    EXPECT_EQ(get(s.d.d, "font"), "revert");
  }
  {
    Style s("all: revert; margin-top: 10px; margin-right: 20px; margin-bottom: 30px; margin-left: 40px");
    EXPECT_EQ(get(s.d.d, "margin"), "10px 20px 30px 40px");
    EXPECT_EQ(get(s.d.d, "all"), "");
    EXPECT_EQ(get(s.d.d, "font"), "revert");
  }
}

TEST(CssomAll, CssTextWritesAllWhenNothingElseIsLeftOfIt) {
  {
    Style s("all: inherit");
    EXPECT_EQ(text_of(s.d.d), "all: inherit;");
  }
  {
    Style s("width: 100px; all: inherit; height: inherit");
    EXPECT_EQ(text_of(s.d.d), "all: inherit;") << "the width is all's now, and the height says what all says";
  }
  {
    Style s("direction: ltr; all: inherit; unicode-bidi: plaintext");
    EXPECT_EQ(text_of(s.d.d), "direction: ltr; all: inherit; unicode-bidi: plaintext;");
  }
  {
    Style s("width: 100px; --a: a; all: inherit; --b: b; height: inherit");
    EXPECT_EQ(text_of(s.d.d), "--a: a; all: inherit; --b: b;");
  }
  {
    std::string css = "all: inherit; ";
    for (const std::string & name : all_longhands()) {
      css += name + ": inherit; ";
    }
    css += "direction: inherit; unicode-bidi: inherit";
    Style s(css);
    EXPECT_EQ(text_of(s.d.d), "all: inherit; direction: inherit; unicode-bidi: inherit;");
  }
  {
    Style s("--foo: bar; all: initial");
    EXPECT_NE(text_of(s.d.d).find("--foo: bar"), std::string::npos) << "all: initial does not exclude a custom property from cssText";
    EXPECT_NE(text_of(s.d.d).find("all: initial"), std::string::npos);
  }
  {
    Style s("all: inherit; width: 1px");
    const std::string t = text_of(s.d.d);
    EXPECT_NE(t.find("width: 1px"), std::string::npos) << t.substr(0, 200);
    EXPECT_EQ(t.find("all:"), std::string::npos) << "a property declared after it takes it out of the shorthand";
  }
}

TEST(CssomAll, LengthAndItemsListEveryLonghand) {
  Style s("all: unset");
  const std::vector<std::string> names = all_longhands();
  EXPECT_EQ(length(s.d.d), names.size()) << "the longhands of all, and not direction, unicode-bidi or a custom property";
  std::vector<std::string> got;
  for (size_t i = 0; i < length(s.d.d); i++) {
    got.push_back(item(s.d.d, i));
  }
  std::vector<std::string> want = names;
  std::sort(got.begin(), got.end());
  std::sort(want.begin(), want.end());
  EXPECT_EQ(got, want);
  EXPECT_EQ(priority(s.d.d, "all"), "");
}

TEST(CssomAll, ImportantIsSharedByAllItsLonghands) {
  Style s("all: inherit !important");
  EXPECT_EQ(priority(s.d.d, "all"), "important");
  EXPECT_EQ(priority(s.d.d, "width"), "important");
  EXPECT_EQ(text_of(s.d.d), "all: inherit !important;");
  EXPECT_EQ(set(s.d.d, "width", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(s.d.d, "width"), "1px") << "setProperty without a priority replaces an important declaration of the same property";
  EXPECT_EQ(priority(s.d.d, "all"), "") << "and then not every longhand is important";
}

TEST(CssomAll, TheChangeEventSaysAllWhenAllIsWhatChanged) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "width", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_FALSE(doc.events[0].all);
  ASSERT_EQ(set(d.d, "all", "inherit").o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(doc.events.size(), 2u);
  EXPECT_TRUE(doc.events[1].all) << "every longhand of all is in the change";
  EXPECT_EQ(doc.events[1].props.size(), all_longhands().size());
  ASSERT_EQ(set(d.d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_FALSE(doc.events[2].all);
  EXPECT_EQ(doc.events[2].props, std::vector<std::string>{"color"});
  ASSERT_EQ(set(d.d, "all", "inherit").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(doc.events[3].props, std::vector<std::string>{"color"}) << "only the one that was different changed";
  EXPECT_FALSE(doc.events[3].all);
  ASSERT_EQ(remove(d.d, "all").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_TRUE(doc.events[4].all);
}

TEST(CssomAll, AllWithAFunctionIsPendingAndReadsAsItsText) {
  Style s("all: var(--k)");
  EXPECT_EQ(get(s.d.d, "all"), "var(--k)");
  EXPECT_EQ(get(s.d.d, "width"), "") << "a longhand left pending by a shorthand has no text";
  EXPECT_EQ(text_of(s.d.d), "all: var(--k);");
}
