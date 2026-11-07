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
 * Allocation failure and `getComputedStyle()` (css_cssom_computed.h): every allocation of making a computed declaration and of reading it for the first time, and of reading
 * it again after the document moved, is failed in turn.
 *
 * A failed read is GTEXT_CSS_E_OOM and leaves the declaration as it was (the next read, with memory, gives what a healthy run gives), the document's generation and
 * counters of live objects unchanged; an allocation the layer tolerates (a style that is not cached) leaves the answer the healthy one; and nothing leaks.
 */

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

using namespace cc;

namespace {

const char * kHtml =
    "<!DOCTYPE html><html><head><style id=s>"
    "div{color:red;margin-top:7px;--k:1px solid red} p{width:50px;background-color:currentcolor;box-shadow:1px 2px 3px currentcolor, 4px 5px blue;animation-duration:auto}"
    "p::before{color:blue} .x{color:color-mix(in srgb, currentcolor, blue)}"
    "@keyframes k{from{height:10px;--c:1px;color:red}to{height:90px;--c:9px;color:blue}} #b{animation:k 10s linear -5s paused;--c:5px;margin-left:var(--c)}"
    "@layer ra{#r{--rp:9px;--rr:ok;margin-left:3px;padding-left:var(--rp)}} @layer rb{#r{margin-left:var(--nope,revert-layer);--rr:var(--nope,revert-layer);padding-left:var(--nope,revert-layer)}}"
    "</style></head><body><div id=d><p id=a class=x>x</p><p id=b>y</p></div><div id=r></div></body></html>";

/// Every name and value of a declaration, as one text.
std::string everything(GTEXT_CSSOM_Decl * d, GTEXT_CSS_Status * status) {
  size_t n = 0;
  *status = gtext_cssom_decl_length(d, &n);
  if (*status != GTEXT_CSS_OK) {
    return "";
  }
  std::string out;
  for (size_t i = 0; i < n; i++) {
    char * name = nullptr;
    size_t nl = 0;
    *status = gtext_cssom_decl_item(d, i, &name, &nl);
    if (*status != GTEXT_CSS_OK) {
      return "";
    }
    const std::string key(name, nl);
    gtext_cssom_buffer_free(gtext_cssom_document_allocator(gtext_cssom_decl_document(d)), name);
    char * value = nullptr;
    size_t vl = 0;
    *status = gtext_cssom_decl_get_property_value(d, key.data(), key.size(), &value, &vl);
    if (*status != GTEXT_CSS_OK) {
      return "";
    }
    out += key + ":" + std::string(value, vl) + ";";
    gtext_cssom_buffer_free(gtext_cssom_document_allocator(gtext_cssom_decl_document(d)), value);
  }
  for (const char * shorthand : {"margin", "border-top-color", "box-shadow", "font"}) {
    char * value = nullptr;
    size_t vl = 0;
    *status = gtext_cssom_decl_get_property_value(d, shorthand, std::strlen(shorthand), &value, &vl);
    if (*status != GTEXT_CSS_OK) {
      return "";
    }
    out += std::string("|") + shorthand + "=" + std::string(value, vl);
    gtext_cssom_buffer_free(gtext_cssom_document_allocator(gtext_cssom_decl_document(d)), value);
  }
  return out;
}

struct Scenario {
  const char * name;
  const char * selector;
  const char * pseudo;
  bool layout;
  bool mutate;   ///< a mutation of the document between the first read and the one that is failed
  bool timeline = false;   ///< the layout gives the animations a time (0), so that `#b` is sampled
};

void sweep(const Scenario & sc) {
  SCOPED_TRACE(sc.name);
  Layout lay;
  lay.timeline = sc.timeline;
  lay.used["width"] = "49px";
  lay.used["margin-top"] = "6px";
  std::string want;
  {
    Cs m(kHtml, [&](GTEXT_CSSOM_Config & c) {
      if (sc.layout) {
        lay.install(c);
      }
    });
    Decl d = m.computed(sc.selector, sc.pseudo);
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    if (sc.mutate) {
      (void)everything(d.d, &st);
      set_attr(m.el(sc.selector), "title", "moved");
    }
    want = everything(d.d, &st);
    ASSERT_EQ(st, GTEXT_CSS_OK);
    ASSERT_FALSE(want.empty());
  }
  size_t failed_runs = 0;
  // one run: the allocation `n` of the phase is failed; false once the phase needs fewer than n allocations
  auto run = [&](size_t n, bool in_make) -> bool {
    FaultAllocator fa;
    bool fired = false;
    {
      Cs m(kHtml, [&](GTEXT_CSSOM_Config & c) {
        if (sc.layout) {
          lay.install(c);
        }
      }, &fa.alloc);
      GTEXT_CSSOM_Decl * made = nullptr;
      const void * h = m.handle(sc.selector);
      const size_t plen = sc.pseudo ? std::strlen(sc.pseudo) : 0;
      GTEXT_CSS_Status st = GTEXT_CSS_OK;
      std::string got;
      if (in_make) {
        fa.arm(n);
        const GTEXT_CSS_Status mk = gtext_cssom_computed_style(m.d(), h, sc.pseudo, plen, &made);
        fa.disarm();
        fired = fa.failed;
        if (mk != GTEXT_CSS_OK) {
          failed_runs++;
          EXPECT_EQ(mk, GTEXT_CSS_E_OOM) << n;
          EXPECT_EQ(made, nullptr);
          made = nullptr;
        }
        else {
          if (sc.mutate) {
            (void)everything(made, &st);
            set_attr(m.el(sc.selector), "title", "moved");
          }
          got = everything(made, &st);
          EXPECT_EQ(st, GTEXT_CSS_OK);
          EXPECT_EQ(got, want) << "allocation " << n << " of making";
        }
      }
      else {
        EXPECT_EQ(gtext_cssom_computed_style(m.d(), h, sc.pseudo, plen, &made), GTEXT_CSS_OK);
        if (sc.mutate) {
          (void)everything(made, &st);
          set_attr(m.el(sc.selector), "title", "moved");
        }
        const uint64_t gen = gtext_cssom_document_generation(m.d());
        size_t len = 0;
        fa.arm(n);
        st = gtext_cssom_decl_length(made, &len);
        fa.disarm();
        fired = fa.failed;
        if (st != GTEXT_CSS_OK) {
          failed_runs++;
          EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
          EXPECT_EQ(gtext_cssom_document_generation(m.d()), gen) << "a failed read moves nothing";
        }
        got = everything(made, &st);   // with memory again: the healthy answer, whether the fault was tolerated or not
        EXPECT_EQ(st, GTEXT_CSS_OK);
        EXPECT_EQ(got, want) << "allocation " << n << " of reading" << (st == GTEXT_CSS_OK && fired ? " (tolerated or recovered)" : "");
      }
      gtext_cssom_decl_release(made);
    }
    EXPECT_TRUE(fa.live.empty()) << "a leak after failing allocation " << n << (in_make ? " of making" : " of reading");
    EXPECT_EQ(fa.bad_frees, 0u);
    return fired;
  };
  for (const bool in_make : {true, false}) {
    // every one of the first allocations, which are the distinct sites (a context, a session, the first record), then every eleventh: the sites past them repeat
    // (a declaration added to the block, a value made) hundreds of times, and a stride that is coprime with their period still lands on each of them in turn
    for (size_t n = 1; n < 100000; n += n < 80 ? 1 : 11) {
      if (!run(n, in_make)) {
        break;
      }
    }
  }
  EXPECT_GT(failed_runs, 20u) << "the sweep reached the allocations of the layer";
}

TEST(CssomComputedOom, ReadingAnElement) {
  sweep({"element", "#a", nullptr, false, false});
}

TEST(CssomComputedOom, ReadingAnElementWhoseDeclarationsRollBackAfterTheirFunctionsWereReplaced) {
  sweep({"rolled back", "#r", nullptr, false, false});
}

TEST(CssomComputedOom, ReadingAPseudoElement) {
  sweep({"pseudo-element", "#a", "::before", false, false});
}

TEST(CssomComputedOom, ReadingWithTheLayout) {
  sweep({"layout", "#b", nullptr, true, false});
}

/* The sampling of an animation: the `@keyframes` rules collected from the sheets, the time asked of the layout, the frames computed, the animated values declared and the style computed again. */
TEST(CssomComputedOom, ReadingAnAnimatedElement) {
  sweep({"animation", "#b", nullptr, true, false, true});
}

TEST(CssomComputedOom, ReadingAnAnimatedElementWhoseTimeWasNotGiven) {
  sweep({"animation without a time", "#b", nullptr, true, false, false});
}

TEST(CssomComputedOom, ReadingAgainAfterTheTreeMoved) {
  sweep({"after a move", "#a", nullptr, false, true});
}

/* gtext_cssom_document_element_released() from a callback of a read writes the element down for the end of the read; when that cannot be done the computed state of the
   document is frozen at the end of the read instead. The read in flight finishes either way, nothing leaks and a declaration made afterwards is healthy. */
TEST(CssomComputedOom, AReleaseFromTheCallbackOfAReadThatCannotBeWrittenDown) {
  struct Probe {
    std::function<void()> act;
    int calls = 0;
    static GTEXT_CSS_Status used(void * user, const void *, const GTEXT_CSS_Selector_List *, GTEXT_CSS_Property_Id, bool * applies, GTEXT_CSS_Value_Set **) {
      Probe * self = static_cast<Probe *>(user);
      if (self->calls++ == 0 && self->act) {
        self->act();
      }
      *applies = false;
      return GTEXT_CSS_OK;
    }
  };
  for (size_t n = 1; n <= 3; n++) {
    SCOPED_TRACE(n);
    Probe probe;
    GTEXT_CSSOM_Layout_Interface iface{};
    iface.size = sizeof iface;
    iface.user = &probe;
    iface.used_value = &Probe::used;
    FaultAllocator fa;
    bool fired = false;
    {
      Cs m(kHtml, [&](GTEXT_CSSOM_Config & c) { c.layout = &iface; }, &fa.alloc);
      Decl a = m.computed("#a");
      Decl b = m.computed("#b");
      EXPECT_EQ(get(b.d, "color"), "rgb(255, 0, 0)");
      probe.calls = 0;
      probe.act = [&] {
        fa.arm(n);
        gtext_cssom_document_element_released(m.d(), m.handle("#a"));
        fired = fa.failed;
        fa.disarm();
      };
      GTEXT_CSS_Status st = GTEXT_CSS_OK;
      EXPECT_FALSE(everything(a.d, &st).empty());
      EXPECT_EQ(st, GTEXT_CSS_OK) << "the read finished";
      EXPECT_GE(probe.calls, 1) << "the callback ran during the read";
      set_attr(m.el("#b"), "class", "x");
      if (fired) {
        EXPECT_EQ(get(b.d, "color"), "rgb(255, 0, 0)") << "the release could not be written down: every declaration is frozen, as at the release of the document";
      }
      else {
        EXPECT_NE(get(b.d, "color"), "rgb(255, 0, 0)") << "a declaration of another element goes on reading";
      }
      Decl again = m.computed("#a");
      EXPECT_FALSE(get(again.d, "margin-top").empty()) << "a declaration made afterwards is healthy";
    }
    EXPECT_TRUE(fa.live.empty()) << "a leak";
    EXPECT_EQ(fa.bad_frees, 0u);
    if (n == 1) {
      EXPECT_TRUE(fired) << "the first allocation of the release was failed";
    }
  }
}

}  // namespace
