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
 * Allocation failure in the declaration object: every call that allocates is swept over the n-th allocation for every n up to the last one it makes. A call that
 * fails fails with GTEXT_CSS_E_OOM and leaves the world exactly as it was (the dump of the declaration, the generation, the events sent, the attribute the binding holds); the
 * call that is not reached by the failure gives the answer of the unswept call; and the allocator holds nothing once everything is released and was never asked to free
 * what it did not make.
 */

#include <functional>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

const char * const kEl = "the element";

/// A world made with a fault allocator: a document, an attribute store, and a declaration (free-standing or an element's).
struct World {
  FaultAllocator fa;
  Attrs attrs;
  std::unique_ptr<Doc> doc;
  GTEXT_CSSOM_Decl * d = nullptr;
  GTEXT_CSSOM_Attribute_Binding binding;
  bool inline_style;

  explicit World(bool is_inline) : inline_style(is_inline) {
    doc.reset(new Doc(
        [&](GTEXT_CSSOM_Config & c) {
          c.tree_version = &Attrs::tree_version;
          c.tree_version_user = &attrs;
        },
        &fa.alloc));
    binding = attrs.binding();
  }
  World(const World &) = delete;
  World & operator=(const World &) = delete;
  ~World() {
    gtext_cssom_decl_release(d);
  }
  void make(const std::string & initial_attribute) {
    if (inline_style) {
      attrs.change(kEl, initial_attribute);
      ASSERT_EQ(gtext_cssom_inline_style(doc->d, kEl, &binding, &d), GTEXT_CSS_OK);
    }
    else {
      ASSERT_EQ(gtext_cssom_decl_new(doc->d, &d), GTEXT_CSS_OK);
      if (!initial_attribute.empty()) {
        ASSERT_EQ(set_text(d, initial_attribute).st, GTEXT_CSS_OK);
      }
    }
  }
  std::string state() {
    return dump(d) + "|gen " + std::to_string(gtext_cssom_document_generation(doc->d)) + "|events " + std::to_string(doc->events.size()) + "|attr " +
        attrs.get(kEl) + "|sets " + std::to_string(attrs.sets);
  }
};

struct Case {
  const char * name;
  bool is_inline;
  const char * initial;
  std::function<GTEXT_CSS_Status(World &)> op;
  bool allocates = true;   ///< False for a call that makes no allocation at all (the sweep then has nothing to fail).
};

/// Run the case with the n-th allocation failed, for n from 1 until the call is not reached by the failure.
void sweep(const Case & c) {
  std::string want_state;
  GTEXT_CSS_Status want_status;
  {
    World w(c.is_inline);
    w.make(c.initial);
    want_status = c.op(w);
    want_state = w.state();
  }
  size_t failures = 0;
  for (size_t n = 1; n < 20000; n++) {
    World w(c.is_inline);
    w.make(c.initial);
    const std::string before = w.state();
    w.fa.arm(n);
    const GTEXT_CSS_Status st = c.op(w);
    w.fa.disarm();
    if (!w.fa.failed) {
      EXPECT_EQ(st, want_status) << c.name << " " << n;
      EXPECT_EQ(w.state(), want_state) << c.name << " " << n;
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << c.name << ": allocation " << n << " " << gtext_css_status_string(st);
    EXPECT_EQ(w.state(), before) << c.name << ": allocation " << n << " changed the world";
    EXPECT_EQ(w.fa.bad_frees, 0u) << c.name;
  }
  if (c.allocates) {
    EXPECT_GT(failures, 0u) << c.name << ": the sweep never failed an allocation";
  }
  // and everything is freed: build the world, run the call unfailed, release all
  {
    World w(c.is_inline);
    w.make(c.initial);
    c.op(w);
    gtext_cssom_decl_release(w.d);
    w.d = nullptr;
    w.doc.reset();
    EXPECT_TRUE(w.fa.live.empty()) << c.name << ": " << w.fa.live.size() << " allocations leak";
    EXPECT_EQ(w.fa.bad_frees, 0u);
  }
}

#define SET(n, v) static_cast<void>(0), [](World & w) { return set(w.d, n, v).st; }

TEST(CssomDeclOom, EveryMutatorIsAtomicUnderAllocationFailureForFreeAndInlineDeclarations) {
  for (const bool inl : {false, true}) {
    const Case cases[] = {
        {"set a longhand", inl, "width: 1px", [](World & w) { return set(w.d, "color", "red").st; }},
        {"replace a longhand", inl, "color: blue; width: 1px", [](World & w) { return set(w.d, "color", "red").st; }},
        {"set the same value", inl, "color: red", [](World & w) { return set(w.d, "color", "red").st; }},
        {"set important", inl, "color: red", [](World & w) { return set(w.d, "color", "red", "important").st; }},
        {"set a shorthand over longhands", inl, "margin-top: 5px; color: red", [](World & w) { return set(w.d, "margin", "1px 2px 3px 4px").st; }},
        {"set a complex shorthand", inl, "", [](World & w) { return set(w.d, "background", "url(a) 1px 2px / 3px 4px no-repeat, red").st; }},
        {"set font", inl, "font-size: 3px", [](World & w) { return set(w.d, "font", "italic bold 12px/1.5 serif").st; }},
        {"set a pending shorthand", inl, "", [](World & w) { return set(w.d, "margin", "var(--x)").st; }},
        {"set a custom property", inl, "--a: 1", [](World & w) { return set(w.d, "--b", " x  y ").st; }},
        {"replace a custom property", inl, "--a: 1", [](World & w) { return set(w.d, "--a", "2").st; }},
        {"set an alias", inl, "", [](World & w) { return set(w.d, "word-wrap", "break-word").st; }},
        {"an invalid value", inl, "color: red", [](World & w) { return set(w.d, "color", "nonsense").st; }},
        {"remove a longhand", inl, "color: red; width: 1px", [](World & w) { return remove(w.d, "color").st; }},
        {"remove a shorthand", inl, "margin: 1px; color: red", [](World & w) { return remove(w.d, "margin").st; }},
        {"remove a custom property", inl, "--a: 1; --b: 2", [](World & w) { return remove(w.d, "--a").st; }},
        {"remove nothing", inl, "color: red", [](World & w) { return remove(w.d, "width").st; }},
        {"set an empty value", inl, "color: red", [](World & w) { return set(w.d, "color", "").st; }},
        {"cssText of a list", inl, "color: red", [](World & w) { return set_text(w.d, "margin: 1px 2px; color: blue !important; --a: 1; width: 3px; color: green; bogus: 1").st; }},
        {"cssText the same", inl, "color: red; width: 1px", [](World & w) { return set_text(w.d, "color: red; width: 1px").st; }},
        {"cssText empty", inl, "color: red", [](World & w) { return set_text(w.d, "").st; }},
        {"cssText a shorthand list", inl, "", [](World & w) { return set_text(w.d, "font: italic bold 12px/1.5 serif; border: 1px solid red; grid-template: [a] \"x y\" 1fr / auto 1fr").st; }},
        {"idl set", inl, "", [](World & w) { return gtext_cssom_decl_idl_set(w.d, "backgroundColor", 15, "red", 3, nullptr); }},
        {"get a value", inl, "margin: 1px 2px", [](World & w) {
           char * out = nullptr;
           size_t n = 0;
           const GTEXT_CSS_Status st = gtext_cssom_decl_get_property_value(w.d, "margin", 6, &out, &n);
           gtext_cssom_buffer_free(&w.doc->core()->alloc, out);
           return st;
         }},
        {"get a longhand", inl, "width: 1px", [](World & w) {
           char * out = nullptr;
           size_t n = 0;
           const GTEXT_CSS_Status st = gtext_cssom_decl_get_property_value(w.d, "width", 5, &out, &n);
           gtext_cssom_buffer_free(&w.doc->core()->alloc, out);
           return st;
         }},
        {"cssText getter", inl, "margin: 1px; color: red; --a: 1", [](World & w) {
           char * out = nullptr;
           size_t n = 0;
           const GTEXT_CSS_Status st = gtext_cssom_decl_css_text(w.d, &out, &n);
           gtext_cssom_buffer_free(&w.doc->core()->alloc, out);
           return st;
         }},
        {"item", inl, "color: red", [](World & w) {
           char * out = nullptr;
           const GTEXT_CSS_Status st = gtext_cssom_decl_item(w.d, 0, &out, nullptr);
           gtext_cssom_buffer_free(&w.doc->core()->alloc, out);
           return st;
         }},
        {"value set", inl, "width: 1px", [](World & w) {
           GTEXT_CSS_Value_Set * v = nullptr;
           bool found = false;
           const GTEXT_CSS_Status st = gtext_cssom_decl_value_set(w.d, "width", 5, &v, nullptr, nullptr, &found);
           gtext_css_value_set_free(v);
           return st;
         }, false},
    };
    for (const Case & c : cases) {
      SCOPED_TRACE(std::string(c.name) + (inl ? " (inline)" : " (free)"));
      sweep(c);
    }
  }
}

// An inline declaration that has to read the attribute again, because it changed behind its back.
TEST(CssomDeclOom, AReloadOfAChangedAttributeIsAtomic) {
  for (size_t n = 1; n < 2000; n++) {
    World w(true);
    w.make("color: red");
    w.attrs.change(kEl, "width: 1px; margin: 2px; --a: 1");
    const std::string before = dump(w.d);
    w.fa.arm(n);
    size_t len = 0;
    const GTEXT_CSS_Status st = gtext_cssom_decl_length(w.d, &len);
    w.fa.disarm();
    if (!w.fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      EXPECT_EQ(len, 6u);
      break;
    }
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(dump(w.d), before) << "allocation " << n << " changed the declaration";
    // and the next call reads it
    EXPECT_EQ(length(w.d), 6u);
  }
}

// The creation of a document, of a declaration and of an element's declaration.
TEST(CssomDeclOom, CreationIsAtomicAndFreesWhatItMade) {
  for (size_t n = 1; n < 200; n++) {
    FaultAllocator fa;
    GTEXT_CSSOM_Config c = gtext_cssom_config_default();
    c.registry = R();
    c.allocator = &fa.alloc;
    GTEXT_CSSOM_Document * d = nullptr;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_cssom_document_new(&c, &d);
    fa.disarm();
    if (!fa.failed) {
      ASSERT_EQ(st, GTEXT_CSS_OK);
      gtext_cssom_document_release(d);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    EXPECT_EQ(st, GTEXT_CSS_E_OOM);
    EXPECT_EQ(d, nullptr);
    EXPECT_TRUE(fa.live.empty()) << n;
  }
  for (const bool inl : {false, true}) {
    for (size_t n = 1; n < 2000; n++) {
      FaultAllocator fa;
      Attrs attrs;
      Doc doc(nullptr, &fa.alloc);
      attrs.change(kEl, "color: red; margin: 1px");
      GTEXT_CSSOM_Attribute_Binding b = attrs.binding();
      GTEXT_CSSOM_Decl * d = nullptr;
      fa.arm(n);
      const GTEXT_CSS_Status st = inl ? gtext_cssom_inline_style(doc.d, kEl, &b, &d) : gtext_cssom_decl_new(doc.d, &d);
      fa.disarm();
      if (!fa.failed) {
        ASSERT_EQ(st, GTEXT_CSS_OK);
        gtext_cssom_decl_release(d);
        doc.release();
        EXPECT_TRUE(fa.live.empty()) << inl;
        break;
      }
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      EXPECT_EQ(d, nullptr);
      GTEXT_CSSOM_Counters cs;
      ASSERT_EQ(gtext_cssom_document_counters(doc.d, &cs), GTEXT_CSS_OK);
      EXPECT_EQ(cs.live_objects, 0u) << n;
      EXPECT_EQ(cs.inline_styles, 0u) << n;
      EXPECT_EQ(cs.live_bytes, 0u) << n;
      doc.release();
      EXPECT_TRUE(fa.live.empty()) << (inl ? "inline " : "free ") << n << ": " << fa.live.size() << " allocations leak";
    }
  }
}

} // namespace
