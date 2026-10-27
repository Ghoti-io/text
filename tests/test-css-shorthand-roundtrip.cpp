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
 * The cssText of a block, read back: for every shorthand of the pin and a list of values, the block made by declaring it, and the blocks made from that
 * by giving one longhand a CSS-wide keyword or a `var()` (a mix of valid and wide or pending longhands, which CSSOM does not write as the
 * shorthand), are written, and the text must be accepted whole, hold the same longhands (their values as CSSOM writes them, their keywords and their
 * importance) and be written again as the same text.
 */

#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

const char * const kValues[] = {
    "1px", "1px 2px", "1px 2px 3px 4px", "red", "none", "auto", "normal", "10px solid red", "red solid 2px", "50%", "1px / 2px", "url(a) repeat-x, red",
    "left top", "span 2 / 3", "1fr / 2fr", "\"a b\" 1fr \"c d\" / auto 1fr", "auto-flow dense 1fr / 100px", "italic bold 12px/1.5 serif", "2 1 10px",
    "pre", "inside none", "0 0 red", "entry, exit", "100px none auto 90deg", "row wrap", "center end", "1px solid", "no-clip", "border-box content-box",
    "a 1s ease-in 2s, b 2s", "10px / 100px", "most-width flip-y, --a", "3", "small-caps",
};

const char * const kLonghandValues[] = {"none", "auto", "normal", "0", "1", "1px", "red", "ellipsis", "no-ellipsis", "-webkit-legacy", "inset", "currentcolor",
    "none, 1px 2px", "1px 2px", "initial", "\"x\""};

/// The effective declarations of a block, by property: kind, keyword or canonical value, importance.
std::map<std::string, std::string> state(const GTEXT_CSS_Declaration_Block * b) {
  std::map<std::string, std::string> m;
  const size_t n = gtext_css_block_count(b);
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Declaration_Item it;
    gtext_css_block_item(b, i, &it);
    GTEXT_CSS_Property_Info info;
    std::string name;
    if (it.custom_name.length) {
      name.assign(it.custom_name.data, it.custom_name.length);
    }
    else if (gtext_css_registry_property_info(R(), it.property, &info)) {
      name.assign(info.name, info.name_length);
    }
    std::string v;
    if (it.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
      v = std::string("wide:") + gtext_css_wide_keyword_name(it.wide);
    }
    else if (it.outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
      v = "pending";
    }
    else if (!it.custom_name.length) {
      char * buf = nullptr;
      size_t len = 0;
      if (gtext_css_block_get_property_value(b, it.property, &buf, &len, nullptr) == GTEXT_CSS_OK) {
        v.assign(buf, len);
        gtext_css_serialize_buffer_free(nullptr, buf);
      }
    }
    m[name] = (it.important ? "!" : "") + v;
  }
  return m;
}

/// The text read into a new block; false (and the declaration) when one of its declarations is refused.
bool reparse(const std::string & css, Block & into, std::string * why) {
  size_t pos = 0;
  while (pos < css.size()) {
    // a declaration ends at the `;` outside parentheses and strings
    int depth = 0;
    char quote = 0;
    size_t end = pos;
    for (; end < css.size(); end++) {
      const char c = css[end];
      if (quote) {
        if (c == '\\') {
          end++;
        }
        else if (c == quote) {
          quote = 0;
        }
      }
      else if (c == '"' || c == '\'') {
        quote = c;
      }
      else if (c == '(' || c == '[') {
        depth++;
      }
      else if (c == ')' || c == ']') {
        depth--;
      }
      else if (c == ';' && depth == 0) {
        break;
      }
    }
    std::string decl = css.substr(pos, end - pos);
    pos = end + 1;
    const size_t colon = decl.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string name = decl.substr(0, colon), value = decl.substr(colon + 1);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    bool important = false;
    const size_t bang = value.rfind(" !important");
    if (bang != std::string::npos && bang + 11 == value.size()) {
      important = true;
      value.erase(bang);
    }
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    const GTEXT_CSS_Status st = gtext_css_block_add_text(into.b, name.data(), name.size(), value.data(), value.size(), important, nullptr, &outcome, nullptr);
    if (st != GTEXT_CSS_OK || (outcome != GTEXT_CSS_PROP_RESULT_VALID && outcome != GTEXT_CSS_PROP_RESULT_WIDE && outcome != GTEXT_CSS_PROP_RESULT_PENDING)) {
      *why = decl;
      return false;
    }
  }
  return true;
}

size_t check(const std::string & declarations, std::vector<std::string> * problems) {
  Block a(R(), declarations);
  if (!a.rejected.empty()) {
    return 0;
  }
  const std::string css = a.css();
  Block b(R());
  std::string why;
  if (!reparse(css, b, &why)) {
    problems->push_back("not accepted: " + declarations + "  =>  {" + css + "}  refuses {" + why + "}");
    return 1;
  }
  if (state(a.b) != state(b.b)) {
    problems->push_back("other longhands: " + declarations + "  =>  {" + css + "}");
  }
  else if (b.css() != css) {
    problems->push_back("not a fixed point: " + declarations + "  =>  {" + css + "}  then  {" + b.css() + "}");
  }
  return 1;
}

TEST(CssShorthandRoundTrip, ABlockOfLonghandsMixingValidAndWideOrPendingIsWrittenAsLonghandsAndReadBackWhole) {
  size_t cases = 0, mixed = 0;
  std::vector<std::string> problems;
  const size_t count = gtext_css_registry_property_count(R());
  for (size_t id = 1; id <= count; id++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(id), &info) || !(info.flags & GTEXT_CSS_PROP_SHORTHAND) ||
        (info.flags & GTEXT_CSS_PROP_ALIAS)) {
      continue;
    }
    const std::string name(info.name, info.name_length);
    for (const char * value : kValues) {
      Block a(R(), name + ": " + value);
      if (!a.rejected.empty() || gtext_css_block_count(a.b) == 0) {
        continue;
      }
      cases += check(name + ": " + value, &problems);
      // each valid longhand in turn becomes `inherit` or a pending value
      std::vector<std::string> longhands;
      for (size_t i = 0; i < gtext_css_block_count(a.b) && longhands.size() < 24; i++) {
        GTEXT_CSS_Declaration_Item it;
        gtext_css_block_item(a.b, i, &it);
        GTEXT_CSS_Property_Info li;
        if (it.outcome == GTEXT_CSS_PROP_RESULT_VALID && gtext_css_registry_property_info(R(), it.property, &li)) {
          longhands.emplace_back(li.name, li.name_length);
        }
      }
      for (const char * word : {"inherit", "var(--x)", "unset"}) {
        for (const std::string & l : longhands) {
          mixed += check(name + ": " + value + "; " + l + ": " + word, &problems);
        }
      }
    }
  }
  EXPECT_GT(cases, 500u);
  EXPECT_GT(mixed, 5000u);
  for (size_t i = 0; i < problems.size() && i < 25; i++) {
    ADD_FAILURE() << problems[i];
  }
  EXPECT_TRUE(problems.empty()) << problems.size() << " of " << (cases + mixed) << " blocks";
}

TEST(CssShorthandRoundTrip, ALonghandWithAValueThatItsShorthandCannotSayIsWrittenAsALonghand) {
  // box-shadow-offset: none, line-clamp's continue with nothing else but its defaults, font-synthesis with a longhand the shorthand names no word for:
  // the shorthand is written only when it says what each longhand holds
  size_t cases = 0;
  std::vector<std::string> problems;
  const size_t count = gtext_css_registry_property_count(R());
  for (size_t id = 1; id <= count; id++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(id), &info) || !(info.flags & GTEXT_CSS_PROP_SHORTHAND) ||
        (info.flags & GTEXT_CSS_PROP_ALIAS)) {
      continue;
    }
    const std::string name(info.name, info.name_length);
    std::vector<std::string> longhands;
    for (size_t k = 0; k < info.longhand_count && k < 24; k++) {
      GTEXT_CSS_Property_Info li;
      if (gtext_css_registry_property_info(R(), gtext_css_registry_longhand(R(), static_cast<GTEXT_CSS_Property_Id>(id), k), &li)) {
        longhands.emplace_back(li.name, li.name_length);
      }
    }
    for (const char * base : {"", "inherit", "1px", "none", "red", "weight style", "3", "1px 2px red"}) {
      const std::string head = base[0] ? name + ": " + base + "; " : std::string();
      for (const std::string & l : longhands) {
        for (const char * v : kLonghandValues) {
          cases += check(head + l + ": " + v, &problems);
        }
        // every longhand at its value, one of them at another
      }
      // two longhands at once, the second after the first
      for (size_t a = 0; a + 1 < longhands.size(); a++) {
        for (const char * v1 : {"none", "ellipsis", "-webkit-legacy", "normal", "auto"}) {
          for (const char * v2 : {"none", "no-ellipsis", "auto", "normal"}) {
            cases += check(head + longhands[a] + ": " + v1 + "; " + longhands[a + 1] + ": " + v2, &problems);
          }
        }
      }
    }
  }
  EXPECT_GT(cases, 3000u);
  for (size_t i = 0; i < problems.size() && i < 25; i++) {
    ADD_FAILURE() << problems[i];
  }
  EXPECT_TRUE(problems.empty()) << problems.size() << " of " << cases << " blocks";
}

TEST(CssShorthandRoundTrip, TheCasesTheReviewFoundAreWrittenWithTheValuesTheBlockHolds) {
  // box-shadow-offset: none is no shadow: `box-shadow: red 0px 0px` is one
  EXPECT_EQ(Block(R(), "box-shadow-color: red; box-shadow-offset: none; box-shadow-blur: 0px; box-shadow-spread: 0px; box-shadow-position: outset").css(),
      "box-shadow-color: red; box-shadow-offset: none; box-shadow-blur: 0px; box-shadow-spread: 0px; box-shadow-position: outset;");
  EXPECT_EQ(Block(R(), "box-shadow: none").css(), "box-shadow: none;");
  // a spread that is written needs its blur written, which the author left out
  EXPECT_EQ(Block(R(), "box-shadow: 1px 2px red; box-shadow-spread: 1px").css(), "box-shadow: red 1px 2px 0px 1px;");
  // `-webkit-legacy` follows at least one of the two components of line-clamp
  EXPECT_EQ(Block(R(), "max-lines: auto; block-ellipsis: no-ellipsis; continue: -webkit-legacy").css(), "line-clamp: auto -webkit-legacy;");
  EXPECT_EQ(Block(R(), "max-lines: 3; block-ellipsis: no-ellipsis; continue: -webkit-legacy").css(), "line-clamp: 3 -webkit-legacy;");
  // a longhand holds a name that is also a keyword of another component: the names are written one by one
  EXPECT_EQ(Block(R(), "animation: none; animation-name: auto").css().find("animation:"), std::string::npos);
  // font-synthesis-position is not among the longhands the pin gives the shorthand: it is written by itself
  EXPECT_EQ(Block(R(), "font-synthesis: weight style small-caps; font-synthesis-position: none").css(),
      "font-synthesis: weight style small-caps; font-synthesis-position: none;");
}

TEST(CssShorthandRoundTrip, AShorthandIsWrittenOnlyWhenItsLonghandsAreAllValidOrAllTheSameKeywordOrAllPending) {
  Block mixed(R(), "margin: 1px; margin-top: inherit");
  EXPECT_EQ(mixed.css(), "margin-right: 1px; margin-bottom: 1px; margin-left: 1px; margin-top: inherit;");
  Block pending(R(), "margin: 1px; margin-top: var(--x)");
  EXPECT_EQ(pending.css(), "margin-right: 1px; margin-bottom: 1px; margin-left: 1px; margin-top: var(--x);");
  EXPECT_EQ(Block(R(), "margin: inherit").css(), "margin: inherit;");
  EXPECT_EQ(Block(R(), "margin: inherit; margin-top: initial").css(), "margin-right: inherit; margin-bottom: inherit; margin-left: inherit; margin-top: initial;");
  EXPECT_EQ(Block(R(), "font-variant: small-caps; font-variant-caps: inherit").css().find("font-variant:"), std::string::npos);
  // nested: the sides of border-width are one keyword, the rest of border are values: border-width alone can be written
  const std::string nested = Block(R(), "border: 1px solid red; border-width: inherit").css();
  EXPECT_NE(nested.find("border-width: inherit"), std::string::npos) << nested;
  EXPECT_EQ(nested.find("border:"), std::string::npos) << nested;
}

} // namespace
