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
 * Where `@apply` looks for a mixin in a document (CSS Functions and Mixins 1, "The name of a @mixin rule is a tree-scoped name"): the sheets of its own tree scope and not those of another, the
 * sheets an `@import` brings in, and what a sheet that is imported depends on.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

size_t records_of(GTEXT_CSSOM_Sheet * s) {
  GTEXT_CSS_Sheet * snap = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_snapshot(s, &snap), GTEXT_CSS_OK);
  const size_t n = snap ? gtext_css_sheet_record_count(snap) : 0;
  gtext_css_sheet_free(snap);
  return n;
}

// "a tree-scoped name": the mixin of the document's sheet is the document's, and a shadow tree's sheet does not find it (nor the document's the shadow tree's).
TEST(CssomMixinScope, AMixinIsFoundInItsOwnTreeScopeOnly) {
  World w;
  int handle = 0;
  Scope shadow(w.d(), &handle, true);
  ASSERT_EQ(shadow.status, GTEXT_CSS_OK);
  Owned lib(w, "@mixin --m { color: red }");
  Owned user(w, "p { @apply --m }");
  Constructed inner(*w.doc, "p { @apply --m } @mixin --n { color: blue }");
  ASSERT_EQ(gtext_cssom_scope_adopted_insert(shadow.s, inner.s, 0, nullptr), GTEXT_CSS_OK);
  Owned outer(w, "p { @apply --n }");
  ASSERT_NE(w.index(), nullptr);
  EXPECT_EQ(records_of(user.s), 1u) << "the document's mixin, applied in the document";
  EXPECT_EQ(records_of(inner.s), 0u) << "not found from the shadow tree";
  EXPECT_EQ(records_of(outer.s), 0u) << "the shadow tree's mixin is not found from the document";
}

// The sheets an @import brings in are read too: the mixin of the imported sheet is found by the sheet that imports it.
TEST(CssomMixinScope, AMixinOfAnImportedSheetIsFound) {
  Importer imp;
  imp.table["lib.css"] = "@mixin --m { color: red }";
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Owned user(w, "@import url(lib.css); p { @apply --m }");
  ASSERT_EQ(user.status, GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "red");
}

// An imported sheet that applies a mixin of another sheet is compiled again, with the sheet that imports it, when that sheet changes.
TEST(CssomMixinScope, AnImportedSheetFollowsTheMixinsItApplies) {
  Importer imp;
  imp.table["child.css"] = "p { @apply --m }";
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Owned root(w, "@import url(child.css);");
  Owned lib(w, "@mixin --m { color: red }");
  ASSERT_EQ(root.status, GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "red");
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(lib.s, "@mixin --m { color: blue }", 25, 1, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "blue");
}

}  // namespace
