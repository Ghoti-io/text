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
 * What the tests of the CSSOM document share (css_cssom_document.h): a Markup page behind the adapter, a document made over it, compiled sheets, conditions objects of a given width,
 * an import callback with a table of texts, and the cascaded value of a property of an element through the document's index.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSSOM_DOCUMENT_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSSOM_DOCUMENT_TEST_SUPPORT_H

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/markup/markup_query.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"
#include "cssom_test_support.h"

namespace cod {

using namespace cot;

/// An allocator that gives every block pages of its own and returns them to the system when the block is freed. A read of a freed block then faults, with or without a
/// sanitizer: the document's allocator lives inside the document, and a delivery that reads it after the last release has read memory that is no longer there.
struct UnmapAllocator {
  GTEXT_Allocator alloc;
  std::map<void *, size_t> sizes;   ///< The block and the bytes the caller asked for.
  UnmapAllocator() {
    alloc.ctx = this;
    alloc.malloc_fn = [](void * ctx, size_t n) -> void * { return static_cast<UnmapAllocator *>(ctx)->take(n); };
    alloc.calloc_fn = [](void * ctx, size_t c, size_t n) -> void * { return static_cast<UnmapAllocator *>(ctx)->take(c * n); };   // fresh pages are zero
    alloc.realloc_fn = [](void * ctx, void * p, size_t n) -> void * {
      auto * self = static_cast<UnmapAllocator *>(ctx);
      void * q = self->take(n);
      if (q && p) {
        const size_t old = self->sizes[p];
        std::memcpy(q, p, old < n ? old : n);
        self->drop(p);
      }
      return q;
    };
    alloc.free_fn = [](void * ctx, void * p) { static_cast<UnmapAllocator *>(ctx)->drop(p); };
  }
  static size_t span(size_t n) {
    const size_t page = 4096;
    return ((n ? n : 1) + page - 1) / page * page;
  }
  void * take(size_t n) {
#ifdef _WIN32
    void * p = VirtualAlloc(nullptr, span(n), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void * p = mmap(nullptr, span(n), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      p = nullptr;
    }
#endif
    if (p) {
      sizes[p] = n;
    }
    return p;
  }
  void drop(void * p) {
    if (!p) {
      return;
    }
    auto it = sizes.find(p);
    ASSERT_TRUE(it != sizes.end());
    const size_t n = it->second;
    sizes.erase(it);
#ifdef _WIN32
    VirtualFree(p, 0, MEM_RELEASE);
    (void)n;
#else
    munmap(p, span(n));
#endif
  }
};

/// A conditions object for a viewport of this width (the registry's defaults otherwise); freed with the object.
struct Cond {
  GTEXT_CSS_Conditions * c = nullptr;
  explicit Cond(double width = 1280, const GTEXT_Allocator * alloc = nullptr, double height = 800) {
    GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(nullptr);
    env.viewport.width = width;
    env.viewport.height = height;
    env.device.width = width;
    env.device.height = height;
    GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
    o.media = &env;
    o.allocator = alloc;
    EXPECT_EQ(gtext_css_conditions_new(R(), &o, &c), GTEXT_CSS_OK);
  }
  Cond(const Cond &) = delete;
  Cond & operator=(const Cond &) = delete;
  ~Cond() {
    gtext_css_conditions_free(c);
  }
};

/// A compiled sheet of a text; freed with the object.
struct Compiled {
  GTEXT_CSS_Sheet * s = nullptr;
  explicit Compiled(const std::string & text) {
    css_test::Parsed p;
    css_test::parse_into(p, gtext_css_parse_stylesheet, text);
    EXPECT_EQ(p.status, GTEXT_CSS_OK);
    EXPECT_EQ(gtext_css_sheet_compile(p.tree, R(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  }
  Compiled(const Compiled &) = delete;
  Compiled & operator=(const Compiled &) = delete;
  ~Compiled() {
    gtext_css_sheet_free(s);
  }
};

/// A Markup page, its adapter and the options a document over it is made with.
struct Page {
  GTEXT_Markup_Document * markup = nullptr;
  GTEXT_CSS_Markup_Adapter * adapter = nullptr;
  GTEXT_CSS_Selector_Match_Options mo{};
  explicit Page(const std::string & html = "<!DOCTYPE html><div id=d class=card><p id=a class=lead>x</p><p id=b>y</p><span id=s>z</span></div>") {
    GTEXT_HTML_Result hr;
    std::memset(&hr, 0, sizeof hr);
    EXPECT_EQ(gtext_html_parse(html.data(), html.size(), nullptr, &markup, &hr), GTEXT_HTML_OK);
    gtext_html_result_free(&hr);
    EXPECT_EQ(gtext_css_markup_adapter_new(markup, nullptr, &adapter), GTEXT_CSS_OK);
    mo = gtext_css_markup_match_options(adapter);
  }
  Page(const Page &) = delete;
  Page & operator=(const Page &) = delete;
  ~Page() {
    gtext_css_markup_adapter_free(adapter);
    gtext_markup_document_free(markup);
  }
  /// The elements matching a selector, in tree order.
  std::vector<GTEXT_Markup_Node *> all(const char * selector) const {
    GTEXT_Markup_Node ** nodes = nullptr;
    size_t n = 0;
    EXPECT_EQ(gtext_markup_query_selector_all(gtext_markup_document_root(markup), selector, std::strlen(selector), nullptr, &nodes, &n, nullptr), GTEXT_MARKUP_OK);
    std::vector<GTEXT_Markup_Node *> out(nodes, nodes + n);
    gtext_markup_query_free(nullptr, nodes);
    return out;
  }
};

/// The cascaded value of a property of the first element matching a selector, through an index ("-" for none): the answer of the cascade the engine would give.
inline std::string cascaded(const Page & page, const GTEXT_CSS_Rule_Index * index, const char * selector, const char * property) {
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  EXPECT_EQ(gtext_css_selector_match_context_new(gtext_css_markup_interface(page.adapter), &page.mo, &ctx), GTEXT_CSS_OK);
  const std::vector<GTEXT_Markup_Node *> els = page.all(selector);
  std::string out = "<no element>";
  if (!els.empty()) {
    cas::Set set(nullptr);
    EXPECT_EQ(gtext_css_rules_collect(index, ctx, gtext_css_markup_handle(els[0]), nullptr, set.s, nullptr, nullptr), GTEXT_CSS_OK);
    cas::Out o(set);
    out = o.value(property);
  }
  gtext_css_selector_match_context_free(ctx);
  return out;
}

/// A CSSOM document over a page with the adapter's element interface and match options; `tweak` adjusts the config. `index()` is the document's index; `value()` the cascaded value of a
/// property of the first element a selector matches, through it.
struct World {
  Page page;
  std::unique_ptr<Doc> doc;
  explicit World(const std::function<void(GTEXT_CSSOM_Config &)> & tweak = nullptr, const GTEXT_Allocator * alloc = nullptr, const std::string & html = "") :
      page(html.empty() ? std::string("<!DOCTYPE html><div id=d class=card><p id=a class=lead>x</p><p id=b>y</p><span id=s>z</span></div>") : html) {
    doc = std::make_unique<Doc>(
        [&](GTEXT_CSSOM_Config & c) {
          c.element_interface = gtext_css_markup_interface(page.adapter);
          c.match_options = &page.mo;
          if (tweak) {
            tweak(c);
          }
        },
        alloc);
  }
  GTEXT_CSSOM_Document * d() const {
    return doc->d;
  }
  const GTEXT_CSS_Rule_Index * index(GTEXT_CSS_Status * status = nullptr) {
    const GTEXT_CSS_Rule_Index * x = nullptr;
    const GTEXT_CSS_Status st = gtext_cssom_document_index(doc->d, &x);
    if (status) {
      *status = st;
    } else {
      EXPECT_EQ(st, GTEXT_CSS_OK);
    }
    return st == GTEXT_CSS_OK ? x : nullptr;
  }
  std::string value(const char * selector, const char * property) {
    const GTEXT_CSS_Rule_Index * x = index();
    return x ? cascaded(page, x, selector, property) : "<no index>";
  }
  GTEXT_CSSOM_Counters counters() const {
    GTEXT_CSSOM_Counters c;
    EXPECT_EQ(gtext_cssom_document_counters(doc->d, &c), GTEXT_CSS_OK);
    return c;
  }
};

/// An owner sheet in the document's scope (parsed, put at the end of `styleSheets`); it is held by the scope and by the object.
struct Owned : Sheet {
  Owned(World & w, const std::string & text, const GTEXT_CSSOM_Sheet_Options * options = nullptr) : Sheet(*w.doc, text, options) {
    EXPECT_EQ(status, GTEXT_CSS_OK);
    GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
    EXPECT_EQ(gtext_cssom_scope_insert_sheet(scope, s, gtext_cssom_scope_sheet_count(scope), nullptr), GTEXT_CSS_OK);
  }
};

/// A scope that releases itself.
struct Scope {
  GTEXT_CSSOM_Scope * s = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  Scope(GTEXT_CSSOM_Document * d, const void * handle, bool shadow, GTEXT_CSSOM_Scope * parent = nullptr, uint32_t context = 0) {
    GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
    o.handle = handle;
    o.shadow = shadow;
    o.parent = parent;
    o.context = context;
    status = gtext_cssom_scope_new(d, &o, &s);
  }
  Scope(const Scope &) = delete;
  Scope & operator=(const Scope &) = delete;
  ~Scope() {
    gtext_cssom_scope_release(s);
  }
};

/// A constructed sheet that releases itself.
struct Constructed {
  GTEXT_CSSOM_Sheet * s = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  explicit Constructed(Doc & doc, const std::string & text = "", const GTEXT_CSSOM_Sheet_Options * options = nullptr) {
    status = gtext_cssom_sheet_new_constructed(doc.d, options, &s);
    if (status == GTEXT_CSS_OK && !text.empty()) {
      status = gtext_cssom_sheet_replace_sync(s, text.data(), text.size(), nullptr);
    }
  }
  Constructed(const Constructed &) = delete;
  Constructed & operator=(const Constructed &) = delete;
  ~Constructed() {
    gtext_cssom_sheet_release(s);
  }
};

/// The import callback with a table: an `href` maps to the text of its sheet, or to "pending" or "failed". Every request is recorded.
struct Importer {
  struct Seen {
    std::string href, media, base, supports, layer;
    bool has_supports = false, has_layer = false;
    size_t depth = 0;
    const void * rule = nullptr;
    const void * sheet = nullptr;
  };
  std::map<std::string, std::string> table;
  std::vector<Seen> seen;
  GTEXT_CSS_Status fail = GTEXT_CSS_OK;           ///< Returned instead of an answer.
  std::function<void(const GTEXT_CSSOM_Import_Request &)> inside;   ///< Runs inside the callback.
  GTEXT_CSSOM_Sheet * given = nullptr;           ///< A sheet to give for any href not in the table (a sheet the test made).
  std::vector<GTEXT_CSSOM_Sheet *> made;

  void install(GTEXT_CSSOM_Config & c) {
    c.import = &Importer::call;
    c.import_user = this;
  }
  static GTEXT_CSS_Status call(void * user, const GTEXT_CSSOM_Import_Request * req, GTEXT_CSSOM_Sheet ** sheet, GTEXT_CSSOM_Import_Outcome * outcome) {
    Importer * self = static_cast<Importer *>(user);
    Seen s;
    s.href.assign(req->href, req->href_length);
    s.media = req->media ? req->media : "";
    s.base = req->base_url ? req->base_url : "";
    s.has_supports = req->supports != nullptr;
    s.supports = req->supports ? req->supports : "";
    s.has_layer = req->layer != nullptr;
    s.layer = req->layer ? req->layer : "";
    s.depth = req->depth;
    s.rule = req->rule;
    s.sheet = req->sheet;
    self->seen.push_back(s);
    if (self->inside) {
      self->inside(*req);
    }
    if (self->fail != GTEXT_CSS_OK) {
      return self->fail;
    }
    auto it = self->table.find(s.href);
    if (it == self->table.end()) {
      if (self->given) {
        *sheet = gtext_cssom_sheet_retain(self->given);
        return GTEXT_CSS_OK;
      }
      *outcome = GTEXT_CSSOM_IMPORT_FAILED;
      return GTEXT_CSS_OK;
    }
    if (it->second == "pending") {
      *outcome = GTEXT_CSSOM_IMPORT_PENDING;
      return GTEXT_CSS_OK;
    }
    if (it->second == "failed") {
      *outcome = GTEXT_CSSOM_IMPORT_FAILED;
      return GTEXT_CSS_OK;
    }
    GTEXT_CSSOM_Sheet * made = nullptr;
    GTEXT_CSSOM_Document * doc = gtext_cssom_sheet_document(req->sheet);
    const GTEXT_CSS_Status st = gtext_cssom_sheet_new_parsed(doc, it->second.data(), it->second.size(), nullptr, &made);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    *sheet = made;
    *outcome = GTEXT_CSSOM_IMPORT_LOADED;
    return GTEXT_CSS_OK;
  }
};

}  // namespace cod

#endif
