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
 * Allocation failure and the Markup binding: every allocation of a sync (and of the one-element path) is failed in turn.
 *
 * A sync is not atomic as a whole (css_cssom_markup.h): a failing call stops with the document consistent and the binding knowing what it has done. So what is asserted is that, after
 * any failure, every sheet of `styleSheets` is the sheet of an element the binding maps to it and no element maps to a sheet that is not listed, that no event was lost, and that the
 * next sync with a healthy allocator ends in exactly the state the unswept sync ends in. Nothing leaks.
 */

#include <gtest/gtest.h>

#include "cssom_markup_test_support.h"

using namespace com;

namespace {

/// Whether the binding and the scope agree: each listed sheet has an owner element whose sheet it is.
void expect_consistent(Mk & m, const std::string & at) {
  std::set<const void *> listed;
  for (size_t i = 0; i < m.count(); i++) {
    GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_sheet_at(m.scope(), i);
    listed.insert(s);
    const void * owner = gtext_cssom_sheet_owner_node(s);
    ASSERT_NE(owner, nullptr) << at;
    GTEXT_CSSOM_Sheet * mine = m.sheet(static_cast<GTEXT_Markup_Node *>(const_cast<void *>(owner)));
    EXPECT_EQ(mine, s) << at << ": the listed sheet is the element's";
    gtext_cssom_sheet_release(mine);
    gtext_cssom_sheet_release(s);
  }
  for (GTEXT_Markup_Node * e : m.page->all("style")) {
    GTEXT_CSSOM_Sheet * s = m.sheet(e);
    if (s) {
      EXPECT_TRUE(listed.count(s)) << at << ": a sheet the binding has for an element is listed";
    }
    gtext_cssom_sheet_release(s);
  }
}

struct Scenario {
  const char * name;
  std::string html;
  std::function<void(Mk &)> edit;   ///< After a healthy first sync: changes to the tree.
  bool first_sync_is_swept;         ///< The sweep is of the first sync (no healthy one before).
};

std::string many() {
  std::string h = "<!DOCTYPE html><html><head>";
  for (int i = 0; i < 6; i++) {
    h += "<style id=s" + std::to_string(i) + " media='screen' title=t" + std::to_string(i) + ">.a" + std::to_string(i) + "{color:red}</style>";
  }
  return h + "</head><body><style id=late>.late{}</style></body></html>";
}

void run(const Scenario & sc) {
  SCOPED_TRACE(sc.name);
  std::string want;
  {
    Mk m(sc.html);
    if (!sc.first_sync_is_swept) {
      ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
    }
    sc.edit(m);
    ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
    want = m.order();
  }
  size_t failures = 0;
  for (size_t n = 1; n < 6000; n++) {
    FaultAllocator fa;
    bool fired = false;
    {
      Mk m(sc.html, nullptr, &fa.alloc);
      ASSERT_EQ(m.status, GTEXT_CSS_OK);
      if (!sc.first_sync_is_swept) {
        ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
      }
      sc.edit(m);
      fa.arm(n);
      const GTEXT_CSS_Status st = m.sync();
      fa.disarm();
      fired = fa.failed;
      if (st != GTEXT_CSS_OK) {
        failures++;
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      expect_consistent(m, "after allocation " + std::to_string(n));
      // the next call goes on, and ends where the unswept one ends
      GTEXT_CSSOM_Outcome o{};
      ASSERT_EQ(m.sync(&o), GTEXT_CSS_OK) << n;
      EXPECT_EQ(m.order(), want) << "after allocation " << n;
      expect_consistent(m, "after the retry of " + std::to_string(n));
      EXPECT_EQ(m.doc->core()->counters.events_lost, 0u);
      EXPECT_EQ(gtext_cssom_document_batch_depth(m.d()), 0u) << "the batch is closed whatever happened";
    }
    EXPECT_TRUE(fa.live.empty()) << "nothing leaks (" << n << ")";
    EXPECT_EQ(fa.bad_frees, 0u);
    if (!fired) {
      break;
    }
  }
  EXPECT_GT(failures, 0u) << "the call allocates";
}

TEST(CssomMarkupOom, TheFirstSyncOfManyStyles) {
  run({"first sync", many(), [](Mk &) {}, true});
}

TEST(CssomMarkupOom, TextChangesRemovalsMovesAndInsertions) {
  run({"edits", many(), [](Mk & m) {
         set_text(m.markup(), m.el("#s1"), ".changed{}");
         detach(m.el("#s3"));
         GTEXT_Markup_Node * moved = m.el("#s0");
         detach(moved);
         append(m.el("body"), moved);
         append(m.el("head"), make_style(m.markup(), ".new{}"));
         set_attr(m.el("#s4"), "media", "print");
         set_attr(m.el("#s5"), "title", "other");
       }, false});
}

TEST(CssomMarkupOom, TheOneElementPathIsAllOrNothingPerStep) {
  const std::string html = many();
  std::string want;
  {
    Mk m(html);
    ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
    set_text(m.markup(), m.el("#s2"), ".changed{}");
    ASSERT_EQ(gtext_cssom_markup_style_element_changed(m.binding, m.el("#s2"), nullptr), GTEXT_CSS_OK);
    want = m.order();
  }
  for (size_t n = 1; n < 2000; n++) {
    FaultAllocator fa;
    bool fired = false;
    {
      Mk m(html, nullptr, &fa.alloc);
      ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
      set_text(m.markup(), m.el("#s2"), ".changed{}");
      fa.arm(n);
      const GTEXT_CSS_Status st = gtext_cssom_markup_style_element_changed(m.binding, m.el("#s2"), nullptr);
      fa.disarm();
      fired = fa.failed;
      EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_OOM);
      expect_consistent(m, "element path at " + std::to_string(n));
      ASSERT_EQ(gtext_cssom_markup_style_element_changed(m.binding, m.el("#s2"), nullptr), GTEXT_CSS_OK);
      EXPECT_EQ(m.order(), want) << n;
    }
    EXPECT_TRUE(fa.live.empty()) << n;
    if (!fired) {
      break;
    }
  }
}

}  // namespace
