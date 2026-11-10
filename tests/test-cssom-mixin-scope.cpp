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

#include <cstddef>
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


/* ---- the document's limits of the expansion -------------------------------------------------------------------------------------------------------------------- */

/// A chain of `n` mixins: `--m<i>` applies `--m<i+1>`, the last sets a declaration, and a rule applies `--m0`.
std::string chain(size_t n) {
  std::string css;
  for (size_t i = 0; i < n; i++) {
    css += "@mixin --m" + std::to_string(i) + " { ";
    css += i + 1 < n ? "@apply --m" + std::to_string(i + 1) + ";" : "--leaf: 1;";
    css += " }\n";
  }
  return css + "p { @apply --m0; }";
}

GTEXT_CSS_Status snapshot_status(Owned & o) {
  GTEXT_CSS_Sheet * snap = nullptr;
  const GTEXT_CSS_Status st = gtext_cssom_sheet_snapshot(o.s, &snap);
  gtext_css_sheet_free(snap);
  return st;
}

// css_sheet.h bounds the expansion with `max_mixin_depth` (16) and `max_mixin_expansions` (65,536); a document compiles its sheets with its own configuration's values, so a document that
// wants other limits has them: the default is the sheet's default, and one more than the limit is the sheet's status.
TEST(CssomMixinLimits, TheDocumentsDefaultsAreTheSheetsDefaults) {
  const GTEXT_CSSOM_Config d = gtext_cssom_config_default();
  EXPECT_EQ(d.max_mixin_depth, 16u);
  EXPECT_EQ(d.max_mixin_expansions, 65536u);
  World w;
  Owned ok(w, chain(16));
  EXPECT_EQ(snapshot_status(ok), GTEXT_CSS_OK);
  Owned over(w, chain(17));
  EXPECT_EQ(snapshot_status(over), GTEXT_CSS_E_LIMIT_RULE_NESTING);
}

TEST(CssomMixinLimits, TheDepthIsTheDocumentsConfiguration) {
  World w([](GTEXT_CSSOM_Config & c) { c.max_mixin_depth = 4; c.max_rule_nesting = 64; });
  Owned ok(w, chain(4));
  EXPECT_EQ(snapshot_status(ok), GTEXT_CSS_OK);
  Owned over(w, chain(5));
  EXPECT_EQ(snapshot_status(over), GTEXT_CSS_E_LIMIT_RULE_NESTING);
}

TEST(CssomMixinLimits, TheDepthIsRaisedToAtMost256) {
  World w([](GTEXT_CSSOM_Config & c) { c.max_mixin_depth = 1000000; c.max_rule_nesting = 4096; });
  Owned ok(w, chain(256));
  EXPECT_EQ(snapshot_status(ok), GTEXT_CSS_OK) << "a limit above 256 is 256, and 256 is allowed";
  Owned over(w, chain(257));
  EXPECT_EQ(snapshot_status(over), GTEXT_CSS_E_LIMIT_RULE_NESTING) << "and not the value that was asked for";
}

TEST(CssomMixinLimits, TheNumberOfExpansionsIsTheDocumentsConfiguration) {
  World w([](GTEXT_CSSOM_Config & c) { c.max_mixin_expansions = 5; });
  Owned five(w, "@mixin --m { --x: 1; } p { @apply --m; @apply --m; @apply --m; @apply --m; @apply --m; }");
  EXPECT_EQ(snapshot_status(five), GTEXT_CSS_OK);
  Owned six(w, "@mixin --n { --x: 1; } p { @apply --n; @apply --n; @apply --n; @apply --n; @apply --n; @apply --n; }");
  EXPECT_EQ(snapshot_status(six), GTEXT_CSS_E_LIMIT_WORK);
}

// "a field left at 0 selects its default, and 0 never means unlimited", and a caller built against the header before these fields (a `size` that ends before them) has the defaults.
TEST(CssomMixinLimits, ZeroAndAnOlderHeaderSelectTheDefaults) {
  {
    World w([](GTEXT_CSSOM_Config & c) { c.max_mixin_depth = 0; c.max_mixin_expansions = 0; });
    Owned ok(w, chain(16));
    EXPECT_EQ(snapshot_status(ok), GTEXT_CSS_OK);
    Owned over(w, chain(17));
    EXPECT_EQ(snapshot_status(over), GTEXT_CSS_E_LIMIT_RULE_NESTING) << "0 is not unlimited";
  }
  {
    World w([](GTEXT_CSSOM_Config & c) {
      c.max_mixin_depth = 2;
      c.max_mixin_expansions = 2;
      c.size = offsetof(GTEXT_CSSOM_Config, max_mixin_depth);
    });
    Owned ok(w, chain(16));
    EXPECT_EQ(snapshot_status(ok), GTEXT_CSS_OK) << "the fields past the caller's size are not read";
  }
}

}  // namespace
