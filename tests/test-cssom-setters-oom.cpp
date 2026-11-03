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
 * Allocation failure in the setters of a rule and in the edits of a typed rule's block: each call is swept over the n-th allocation for every n up to the last one it makes. A call that fails
 * fails with GTEXT_CSS_E_OOM and leaves the sheet, its versions, its ids, the counts of the document and its cache as they were (the dump before equals the dump after; the same
 * call made again gives the result of the unswept call, ids included); one the failure does not reach gives that result; nothing leaks once the document is released.
 */

#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

bool needs_block(const char * name) {
  const std::string n = name;
  return n.find("a page") == 0 || n.find("a keyframe property") == 0 || n.find("a font-face") == 0 || n.find("the cssText") == 0 || n.find("the removal") == 0;
}

struct Scenario {
  const char * name;
  const char * css;
  std::function<GTEXT_CSS_Status(Sheet &)> op;
  size_t min_failures = 3;   ///< The allocations the call is known to make at least.
  bool constructed = false;  ///< A constructed sheet whose rules are first set from `css`.
  std::function<void(Sheet &)> prepare = nullptr;   ///< Run on the sheet before the call is measured (and before the state it is compared with is taken).
};

GTEXT_CSS_Status sel(Sheet & s, std::initializer_list<size_t> p, const char * text) {
  GTEXT_CSSOM_Outcome o;
  return gtext_cssom_rule_set_selector_text(path(s, p).r, text, std::strlen(text), &o);
}

GTEXT_CSS_Status attr_set(Sheet & s, std::initializer_list<size_t> p, const char * name, const char * value) {
  GTEXT_CSSOM_Outcome o;
  return gtext_cssom_rule_set_attribute(path(s, p).r, name, std::strlen(name), value, std::strlen(value), &o);
}

GTEXT_CSS_Status prop(Sheet & s, std::initializer_list<size_t> p, const char * name, const char * value) {
  Rule r = path(s, p);
  Decl d = style_of(r.r);
  if (!d.d) {
    return GTEXT_CSS_E_OOM;
  }
  return set(d.d, name, value).st;
}

GTEXT_CSS_Status media_set(Sheet & s, std::initializer_list<size_t> p, const char * text) {
  GTEXT_CSSOM_Media_List * m = nullptr;
  GTEXT_CSS_Status st = p.size() ? gtext_cssom_rule_media(path(s, p).r, &m) : gtext_cssom_sheet_media(s.s, &m);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Outcome o;
  st = gtext_cssom_media_set_text(m, text, std::strlen(text), &o);
  gtext_cssom_media_release(m);
  return st;
}

GTEXT_CSS_Status media_append(Sheet & s, std::initializer_list<size_t> p, const char * text) {
  GTEXT_CSSOM_Media_List * m = nullptr;
  GTEXT_CSS_Status st = gtext_cssom_rule_media(path(s, p).r, &m);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Outcome o;
  st = gtext_cssom_media_append(m, text, std::strlen(text), &o);
  gtext_cssom_media_release(m);
  return st;
}

void make_sheet(Sheet & s, Doc & doc, const Scenario & sc) {
  if (!sc.constructed) {
    s.status = gtext_cssom_sheet_new_parsed(doc.d, sc.css, std::strlen(sc.css), nullptr, &s.s);
    if (s.status == GTEXT_CSS_OK && sc.prepare) {
      sc.prepare(s);
    }
    return;
  }
  s.status = gtext_cssom_sheet_new_constructed(doc.d, nullptr, &s.s);
  if (s.status == GTEXT_CSS_OK) {
    s.status = gtext_cssom_sheet_replace_sync(s.s, sc.css, std::strlen(sc.css), nullptr);
  }
}

const Scenario kScenarios[] = {
    {"mediaText of a sheet", "a{}", [](Sheet & s) { return media_set(s, {}, "screen, (min-width: 1px)"); }, 1},
    {"mediaText of a sheet that ends in an unclosed parenthesis", "a{}", [](Sheet & s) { return media_set(s, {}, "print, screen and ( a , (b ( c"); }, 1},
    {"mediaText of a media rule", "@media screen { a {} }", [](Sheet & s) { return media_set(s, {0}, "print, tv"); }},
    {"mediaText of a media rule that ends in an unclosed parenthesis", "@media screen { a {} }", [](Sheet & s) { return media_set(s, {0}, "print, screen and ( a , (b ( c"); }},
    {"mediaText of an import that ends in an unclosed parenthesis", "@import url(a.css) screen;", [](Sheet & s) { return media_set(s, {0}, "tv, (a:b"); }},
    {"appendMedium to a media rule that has an unclosed parenthesis", "@media screen { a {} }", [](Sheet & s) { return media_append(s, {0}, "(x ( y"); }, 3, false,
     [](Sheet & s) { EXPECT_EQ(media_set(s, {0}, "screen and (a"), GTEXT_CSS_OK); }},
    {"appendMedium to an import", "@import url(a.css) screen;", [](Sheet & s) { return media_append(s, {0}, "print"); }},
    {"replaceSync", "a { b {} } @media print { c {} }", [](Sheet & s) {
       return gtext_cssom_sheet_replace_sync(s.s, "x { y {} } z {} @import 'q';", 27, nullptr);
     }, 3, true},

    {"selectorText of a style rule", "a, b { color: red; & c {} }", [](Sheet & s) { return sel(s, {0}, "d > e, f"); }},
    {"selectorText of a nested rule", "a { b { color: red } }", [](Sheet & s) { return sel(s, {0, 0}, "> c"); }},
    {"selectorText of a page rule", "@page { size: a3 }", [](Sheet & s) { return sel(s, {0}, ":first"); }},
    {"keyText", "@keyframes k { from { color: red } }", [](Sheet & s) {
       GTEXT_CSSOM_Outcome o;
       return gtext_cssom_rule_set_key_text(path(s, {0, 0}).r, "10%, 90%", 8, &o);
     }},
    {"a counter style descriptor", "@counter-style c { system: cyclic; symbols: a b }", [](Sheet & s) { return attr_set(s, {0}, "symbols", "x y z"); }},
    {"a keyframes name", "@keyframes k { from { color: red } }", [](Sheet & s) { return attr_set(s, {0}, "name", "x y"); }},
    {"a page size", "@page { size: a3 }", [](Sheet & s) { return prop(s, {0}, "size", "portrait"); }},
    {"a page margin", "@page { size: a3 }", [](Sheet & s) { return prop(s, {0}, "margin", "1px 2px"); }},
    {"a keyframe property", "@keyframes k { from { color: red } }", [](Sheet & s) { return prop(s, {0, 0}, "margin-top", "1px"); }},
    {"a font-face descriptor", "@font-face { font-family: A }", [](Sheet & s) { return prop(s, {0}, "font-display", "swap"); }},
    {"the cssText of a typed block", "@page { size: a3 }", [](Sheet & s) {
       Rule r = path(s, {0});
       Decl d = style_of(r.r);
       return d.d ? set_text(d.d, "margin-top: 1px; size: portrait").st : GTEXT_CSS_E_OOM;
     }},
    {"the removal from a typed block", "@page { size: a3; margin: 1px }", [](Sheet & s) {
       Rule r = path(s, {0});
       Decl d = style_of(r.r);
       return d.d ? remove(d.d, "size").st : GTEXT_CSS_E_OOM;
     }},
    {"set() of a feature value map", "@font-feature-values F { @swash { a: 1 } @styleset { b: 2 3 } }", [](Sheet & s) {
       const uint32_t v[2] = {4, 5};
       GTEXT_CSSOM_Outcome o;
       return gtext_cssom_feature_map_set(path(s, {0}).r, GTEXT_CSS_FEATURE_STYLESET, "c", 1, v, 2, &o);
     }},
    {"delete() of a feature value map", "@font-feature-values F { @swash { a: 1; b: 2 } }", [](Sheet & s) {
       GTEXT_CSSOM_Outcome o;
       bool deleted = false;
       return gtext_cssom_feature_map_delete(path(s, {0}).r, GTEXT_CSS_FEATURE_SWASH, "a", 1, &deleted, &o);
     }},
    {"clear() of a feature value map", "@font-feature-values F { @swash { a: 1; b: 2 } @ornaments { o: 1 } }", [](Sheet & s) {
       GTEXT_CSSOM_Outcome o;
       return gtext_cssom_feature_map_clear(path(s, {0}).r, GTEXT_CSS_FEATURE_SWASH, &o);
     }},
    {"fontFamily keeps the feature blocks", "@font-feature-values F { @swash { a: 1 } }", [](Sheet & s) { return attr_set(s, {0}, "fontFamily", "G, H"); }},
};

/// The sheet's dump, and for the scenarios that change a sheet's media, the text of its list as read (the dump does not say it).
std::string snap(Sheet & s, const Scenario & sc) {
  std::string out = dump(s.s);
  if (std::string(sc.name).find("mediaText of a sheet") == 0) {
    GTEXT_CSSOM_Media_List * m = nullptr;
    if (gtext_cssom_sheet_media(s.s, &m) == GTEXT_CSS_OK) {
      char * t = nullptr;
      size_t n = 0;
      if (gtext_cssom_media_text(m, &t, &n) == GTEXT_CSS_OK) {
        out += "\nmedia: " + std::string(t, n);
        gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(m)->core->alloc, t);
      }
      gtext_cssom_media_release(m);
    }
  }
  return out;
}

TEST(CssomSettersOom, EveryMutatorIsAllOrNothing) {
  for (const Scenario & sc : kScenarios) {
    std::string want;
    GTEXT_CSS_Status want_status = GTEXT_CSS_OK;
    {
      FaultAllocator fa;
      Doc doc(nullptr, &fa.alloc);
      Sheet s;
      make_sheet(s, doc, sc);
      ASSERT_EQ(s.status, GTEXT_CSS_OK) << sc.name;
      want_status = sc.op(s);
      ASSERT_EQ(want_status, GTEXT_CSS_OK) << sc.name;
      want = snap(s, sc);
    }
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      {
        Doc doc(nullptr, &fa.alloc);
        Sheet s;
        make_sheet(s, doc, sc);
        ASSERT_EQ(s.status, GTEXT_CSS_OK) << sc.name;
        if (std::string(sc.name).find("mediaText of a") == 0 || std::string(sc.name).find("appendMedium") == 0) {
          /* the list object is made the first time it is asked for and stays: not what the call is measured on */
          GTEXT_CSSOM_Media_List * m = nullptr;
          if (std::string(sc.name).find("sheet") != std::string::npos) {
            ASSERT_EQ(gtext_cssom_sheet_media(s.s, &m), GTEXT_CSS_OK);
          }
          else {
            ASSERT_EQ(gtext_cssom_rule_media(path(s, {0}).r, &m), GTEXT_CSS_OK);
          }
          gtext_cssom_media_release(m);
        }
        if (needs_block(sc.name)) {
          const bool in_keyframes = std::string(sc.css).find("@keyframes") != std::string::npos;
          Rule r = in_keyframes ? path(s, {0, 0}) : path(s, {0});
          Decl d = style_of(r.r);
        }
        const std::string before = snap(s, sc);
        const uint64_t generation = gtext_cssom_document_generation(doc.d);
        const uint64_t version = gtext_cssom_sheet_version(s.s);
        GTEXT_CSSOM_Counters c0;
        ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c0), GTEXT_CSS_OK);
        const size_t events = doc.events.size();
        fa.arm(n);
        const GTEXT_CSS_Status st = sc.op(s);
        fa.disarm();
        if (!fa.failed) {
          ASSERT_EQ(st, GTEXT_CSS_OK) << sc.name << " at " << n;
          EXPECT_EQ(snap(s, sc), want) << sc.name << ": the sweep ended at " << n;
          break;
        }
        failures++;
        ASSERT_EQ(st, GTEXT_CSS_E_OOM) << sc.name << ": allocation " << n << " " << gtext_css_status_string(st);
        EXPECT_EQ(snap(s, sc), before) << sc.name << ": allocation " << n;
        EXPECT_EQ(gtext_cssom_document_generation(doc.d), generation) << sc.name << " " << n;
        EXPECT_EQ(gtext_cssom_sheet_version(s.s), version) << sc.name << " " << n;
        EXPECT_EQ(doc.events.size(), events) << sc.name << " " << n << ": no event for a call that failed";
        GTEXT_CSSOM_Counters c1;
        ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c1), GTEXT_CSS_OK);
        EXPECT_EQ(c1.live_objects, c0.live_objects) << sc.name << " " << n;
        EXPECT_EQ(c1.live_bytes, c0.live_bytes) << sc.name << " " << n;
        EXPECT_EQ(c1.tree_bytes, c0.tree_bytes) << sc.name << " " << n;
        // and the same call again, now that nothing fails, is the unswept call: the ids are the ones it would have had
        EXPECT_EQ(sc.op(s), GTEXT_CSS_OK) << sc.name << " " << n;
        EXPECT_EQ(snap(s, sc), want) << sc.name << ": the retry after allocation " << n;
      }
      EXPECT_TRUE(fa.live.empty()) << sc.name << ": allocation " << n << " leaked";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GE(failures, sc.min_failures) << sc.name;
  }
}

/* A rule whose block was fetched: the block is a view of the rule's syntax, and a setter that fails leaves both as they were. The sheet's dump says nothing of the view. */
TEST(CssomSettersOom, TheBlockOfATypedRuleFetchedBeforeIsUnchangedByAFailedSetter) {
  struct Case {
    const char * name;
    const char * css;
    std::function<GTEXT_CSS_Status(Sheet &)> op;
    const char * view_has = nullptr;   ///< What the block must say after the call, said independently of the library's own answer.
  };
  const Case cases[] = {
      {"a counter style descriptor", "@counter-style c { system: cyclic; symbols: a b }", [](Sheet & s) { return attr_set(s, {0}, "symbols", "x y z"); }, "symbols: x y z"},
      {"a counter style name", "@counter-style c { system: cyclic; symbols: a b }", [](Sheet & s) { return attr_set(s, {0}, "name", "d"); }},
      {"selectorText of a page rule", "@page { size: a3; margin: 1px }", [](Sheet & s) { return sel(s, {0}, ":first"); }},
      {"keyText", "@keyframes k { from { color: red; margin-top: 1px } }", [](Sheet & s) {
         GTEXT_CSSOM_Outcome o;
         return gtext_cssom_rule_set_key_text(path(s, {0, 0}).r, "10%, 90%", 8, &o);
       }},
  };
  for (const Case & c : cases) {
    const bool nested = std::string(c.css).find("@keyframes") != std::string::npos && std::string(c.name).find("keyText") != std::string::npos;
    std::string want_view;
    {
      FaultAllocator fa;
      Doc doc(nullptr, &fa.alloc);
      Sheet s(doc, c.css);
      ASSERT_EQ(s.status, GTEXT_CSS_OK) << c.name;
      Rule r = nested ? path(s, {0, 0}) : path(s, {0});
      Decl d = style_of(r.r);
      ASSERT_TRUE(d.d) << c.name;
      ASSERT_EQ(c.op(s), GTEXT_CSS_OK) << c.name;
      want_view = text_of(d.d);
      if (c.view_has) {
        EXPECT_NE(want_view.find(c.view_has), std::string::npos) << c.name << ": " << want_view;
      }
    }
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      {
        Doc doc(nullptr, &fa.alloc);
        Sheet s(doc, c.css);
        ASSERT_EQ(s.status, GTEXT_CSS_OK) << c.name;
        Rule r = nested ? path(s, {0, 0}) : path(s, {0});
        Decl d = style_of(r.r);
        ASSERT_TRUE(d.d) << c.name;
        const std::string view_before = text_of(d.d);
        const std::string dump_before = dump(s.s);
        fa.arm(n);
        const GTEXT_CSS_Status st = c.op(s);
        fa.disarm();
        if (!fa.failed) {
          ASSERT_EQ(st, GTEXT_CSS_OK) << c.name << " at " << n;
          EXPECT_EQ(text_of(d.d), want_view) << c.name;
          break;
        }
        failures++;
        EXPECT_EQ(dump(s.s), dump_before) << c.name << ": allocation " << n << " (the call returned " << gtext_css_status_string(st) << ")";
        EXPECT_EQ(text_of(d.d), view_before) << c.name << ": allocation " << n << ": the block is still the view of the unchanged rule";
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << c.name << " " << n;
        EXPECT_EQ(c.op(s), GTEXT_CSS_OK) << c.name << " " << n;
        EXPECT_EQ(text_of(d.d), want_view) << c.name << ": the retry after allocation " << n;
      }
      EXPECT_TRUE(fa.live.empty()) << c.name << ": allocation " << n << " leaked";
    }
    EXPECT_GE(failures, 3u) << c.name;
  }
}

}  // namespace
