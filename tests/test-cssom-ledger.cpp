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
 * The rows of tools/css/cssom_ledger.tsv that say a whole family of attributes is implemented are claims, and this program checks them: the attributes of the three descriptor
 * interfaces of the pinned IDL (CSSPageDescriptors, CSSFontFaceDescriptors, CSSPositionTryDescriptors), each of which the block of its rule must have, readable and writable.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

std::vector<std::string> words(const char * s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char * p = s;; p++) {
    if (*p == ' ' || *p == '\0') {
      if (!cur.empty()) {
        out.push_back(cur);
      }
      cur.clear();
      if (!*p) {
        break;
      }
    }
    else {
      cur += *p;
    }
  }
  return out;
}

void expect_all(const char * css_text, const char * names, const char * absent) {
  Doc doc;
  Sheet s(doc, css_text);
  Rule r = path(s, {0});
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  Decl d = style_of(r.r, &st);
  ASSERT_EQ(st, GTEXT_CSS_OK) << css_text;
  for (const std::string & n : words(names)) {
    EXPECT_TRUE(gtext_cssom_decl_has_attribute(d.d, n.data(), n.size())) << css_text << ": " << n;
    char * out = nullptr;
    size_t len = 0;
    EXPECT_EQ(gtext_cssom_decl_idl_get(d.d, n.data(), n.size(), &out, &len), GTEXT_CSS_OK) << n;
    if (out) {
      gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(d.d)->core->alloc, out);
    }
  }
  for (const std::string & n : words(absent)) {
    EXPECT_FALSE(gtext_cssom_decl_has_attribute(d.d, n.data(), n.size())) << css_text << ": " << n << " is an `out` row of the ledger (a short name of the newer interface, no served sentence)";
  }
}

}  // namespace

TEST(CssomLedger, TheAttributesOfCSSPageDescriptors) {
  expect_all("@page { }", "margin marginTop marginRight marginBottom marginLeft size pageOrientation marks bleed", "top right bottom left orientation");
}

TEST(CssomLedger, TheAttributesOfCSSFontFaceDescriptors) {
  expect_all("@font-face { }",
      "src fontFamily fontStyle fontWeight fontStretch fontWidth fontSize sizeAdjust unicodeRange fontFeatureSettings "
      "fontVariationSettings fontNamedInstance fontDisplay fontLanguageOverride ascentOverride descentOverride lineGapOverride superscriptPositionOverride "
      "subscriptPositionOverride superscriptSizeOverride subscriptSizeOverride",
      "family style weight stretch width size adjust range settings instance display override");
}

TEST(CssomLedger, TheAttributesOfCSSPositionTryDescriptors) {
  expect_all("@position-try --x { }",
      "margin marginTop marginRight marginBottom marginLeft marginBlock marginBlockStart marginBlockEnd marginInline marginInlineStart marginInlineEnd top right bottom left "
      "inset insetBlock insetBlockStart insetBlockEnd insetInline insetInlineStart insetInlineEnd width minWidth maxWidth height minHeight maxHeight blockSize minBlockSize maxBlockSize "
      "inlineSize minInlineSize maxInlineSize placeSelf alignSelf justifySelf positionAnchor positionArea",
      "anchor area block end inline self size start");
}
