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
 * The dump of a sheet, a rule and a declaration is plain text that depends on the calls and on nothing else, and its hash is the SHA-256 of it. These hashes were taken on x86-64 and are
 * pinned here, so that the cross-architecture runs of this program (32-bit x86, AArch64, big-endian s390x, and win64 under wine) fail if a dump differs by one byte on any of them: the
 * layout of a struct, the width of a size_t, an endianness or a locale must not reach it.
 */

#include <cstdio>
#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

std::string hex(const unsigned char * h) {
  char out[2 * GTEXT_CSSOM_HASH_LENGTH + 1];
  for (size_t i = 0; i < GTEXT_CSSOM_HASH_LENGTH; i++) {
    std::snprintf(out + 2 * i, 3, "%02x", h[i]);
  }
  return out;
}

std::string sheet_hash(const char * css_text, bool edit) {
  Doc doc;
  Sheet s(doc, css_text);
  if (edit) {
    EXPECT_EQ(insert_rule(s.s, "z { color: red; w { margin: 1px } }", 0).st, GTEXT_CSS_OK);
    EXPECT_EQ(delete_rule(s.s, 1), GTEXT_CSS_OK);
    Rule r = path(s, {0});
    Result res;
    res.st = gtext_cssom_rule_set_selector_text(r.r, "y, x > w", 8, &res.o);
    EXPECT_EQ(res.st, GTEXT_CSS_OK);
  }
  unsigned char h[GTEXT_CSSOM_HASH_LENGTH];
  EXPECT_EQ(gtext_cssom_sheet_hash(s.s, h), GTEXT_CSS_OK);
  return hex(h);
}

}  // namespace

TEST(CssomDumpBytes, TheHashOfASheetIsTheSameOnEveryTarget) {
  EXPECT_EQ(sheet_hash("a { color: red } b { margin: 1px 2px } @media print { c { width: 1px } }", false), "db19363c4a51066f7c2c8574af23ccbf231346bba47f1ee305e56a76681185a8");
  EXPECT_EQ(sheet_hash("@import url(a.css) screen; @namespace x url(u); x|a { b { color: blue } } @keyframes k { from { opacity: 0 } 50% { opacity: .5 } to { opacity: 1 } }", false), "1a4ff1b87a05d6359c87b570c148c4e2abe3bc1277fde0364c0f5648870148d5");
  EXPECT_EQ(sheet_hash("@font-face { font-family: f; src: url(a.woff2) } @page :first { margin: 1px @top-left { content: \"x\" } } @counter-style c { system: cyclic; symbols: a b }", false), "3d6df1641401c9a16ac07effe065dafb937321f4b870a6b165d894c3cdbe2896");
  EXPECT_EQ(sheet_hash("a { color: red } b { margin: 1px } c {}", true), "7e0b093ab3dfaba166db700e8947e68aaebc9e97373ae79830dc642a7593b29a");
  EXPECT_EQ(sheet_hash("@layer l { a { color: red } } @supports (display: grid) { b { display: grid } } @container (min-width: 1px) { c { margin: 0 } } @scope (.s) { d {} }", false), "bbfbbb7ba70c6c5b17897e1f10cda4b7d023ccd17f0d6463402cd193fb5ebb65");
  EXPECT_EQ(sheet_hash("", false), "4321b1ab93006ee944c39a139d2ec49b780a69481f4f31f96e3afde19319944e");
}

TEST(CssomDumpBytes, TheHashOfADeclarationIsTheSameOnEveryTarget) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "color: red; margin: 1px 2px; --x: y z; background: red url(a.png) !important; font: 12px/1.5 serif").st, GTEXT_CSS_OK);
  ASSERT_EQ(set(d.d, "margin-top", "5px").st, GTEXT_CSS_OK);
  unsigned char h[GTEXT_CSSOM_HASH_LENGTH];
  ASSERT_EQ(gtext_cssom_decl_hash(d.d, h), GTEXT_CSS_OK);
  EXPECT_EQ(hex(h), "7c4ca0b920ff1784fd34c993647811b7c38f7d896df5bcd6784583124a9e5f9a");
}
