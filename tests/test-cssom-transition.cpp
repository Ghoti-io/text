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
 * `getComputedStyle()` under CSS Transitions (css_cssom_core.h, GTEXT_CSSOM_Layout_Interface::transitions and ::transition_version; css_cssom_computed.h, the transition tracker and
 * gtext_cssom_document_after_change_style()): the value of a property that is transitioning is its current value at the tracker's time, a transition starts on a style change event and not
 * on a read, and a transition of an inherited property is inherited.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

using namespace cc;

namespace {

/// A page, a tracker over its document and the layout hooks that give the tracker's sets.
struct Tr {
  GTEXT_CSSOM_Layout_Interface iface{};
  GTEXT_CSSOM_Transition_Tracker * tracker = nullptr;
  const GTEXT_Allocator * alloc_;
  Cs m;
  explicit Tr(const std::string & html, bool hooks = true, const GTEXT_Allocator * alloc = nullptr, bool version = true)
      : alloc_(alloc), m(html, [this, hooks, version](GTEXT_CSSOM_Config & c) {
          iface.size = sizeof iface;
          iface.user = this;
          iface.transitions = &Tr::sets;
          iface.transition_version = version ? &Tr::ver : nullptr;   /* the document copies the interface when it is made: a hook cleared afterwards is still called */
          if (hooks) {
            c.layout = &iface;
          }
        }, alloc) {
    EXPECT_EQ(gtext_cssom_transition_tracker_new(m.doc->d, alloc_, &tracker), GTEXT_CSS_OK);
  }
  Tr(const Tr &) = delete;
  Tr & operator=(const Tr &) = delete;
  ~Tr() {
    gtext_cssom_transition_tracker_free(tracker);
  }
  static GTEXT_CSS_Status sets(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Transition_Set ** set, double * ms) {
    Tr * self = static_cast<Tr *>(user);
    if (!self->tracker) {
      *set = nullptr;
      *ms = 0;
      return GTEXT_CSS_OK;
    }
    return gtext_cssom_transition_tracker_lookup(self->tracker, element, pseudo, set, ms);
  }
  static uint64_t ver(void * user) {
    return gtext_cssom_transition_tracker_version(static_cast<Tr *>(user)->tracker);
  }
  void flush(const char * selector, const char * pseudo = nullptr) {
    ASSERT_EQ(gtext_cssom_transition_tracker_flush(tracker, m.handle(selector), pseudo, pseudo ? std::strlen(pseudo) : 0), GTEXT_CSS_OK);
  }
  void at(double ms) {
    gtext_cssom_transition_tracker_set_time(tracker, ms);
  }
  void set_class(const char * selector, const std::string & value) {
    GTEXT_Markup_Node * e = m.el(selector);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "class", value.data(), value.size()), GTEXT_MARKUP_OK);
  }
  std::string width(const char * selector, const char * pseudo = nullptr, const char * property = "width") {
    Decl d = m.computed(selector, pseudo);
    return d.d ? get(d.d, property) : "<no declaration>";
  }
};

std::string page(const std::string & css, const std::string & body = "<div id=t></div>") {
  return "<!DOCTYPE html><html><head><style id=s>" + css + "</style></head><body>" + body + "</body></html>";
}

const char * kBox = "#t { width: 100px; transition: width 1s linear } #t.on { width: 200px }";

/* "When a style change event occurs, implementations must start transitions based on the computed values that changed in that event": the change of class is the change, the flush is the
   event, and the read after it shows the current value at the time the caller gives. */
TEST(CssomTransition, AClassChangeStartsATransitionAtTheNextEventAndTheReadShowsItsCurrentValue) {
  Tr t(page(kBox));
  t.at(0);
  t.flush("#t");
  EXPECT_EQ(t.width("#t"), "100px");
  t.set_class("#t", "on");
  t.flush("#t");
  EXPECT_EQ(t.width("#t"), "100px") << "at the start time the current value is the start value";
  t.at(250);
  EXPECT_EQ(t.width("#t"), "125px") << "the time moved, and the tracker's version moved the key of the cached style with it";
  t.at(500);
  EXPECT_EQ(t.width("#t"), "150px");
  t.at(1000);
  EXPECT_EQ(t.width("#t"), "200px");
}

/* A read is not an event: without a flush the new value shows at once, as it does in a document with no transitions. */
TEST(CssomTransition, WithoutAnEventTheNewValueIsTheValue) {
  Tr t(page(kBox));
  t.at(0);
  t.flush("#t");
  t.set_class("#t", "on");
  EXPECT_EQ(t.width("#t"), "200px");
}

/* "If an element is not in the document during that style change event or was not in the document during the previous style change event, then transitions are not started for that
   element": the first event an element is in starts nothing. */
TEST(CssomTransition, TheFirstEventOfAnElementStartsNothing) {
  Tr t(page(kBox, "<div id=t class=on></div>"));
  t.at(0);
  t.flush("#t");
  EXPECT_EQ(t.width("#t"), "200px");
  t.at(100);
  EXPECT_EQ(t.width("#t"), "200px");
}

/* The pseudo-element is an element for this: the page transition-test of css-pseudo, `all 2s -1s linear` on ::marker, from one value to another, is half way at once. */
TEST(CssomTransition, APseudoElementTransitionsToo) {
  Tr t(page("li { display: list-item } li::marker { transition: all 2s -1s linear; color: rgb(0, 0, 0) } li.to::marker { color: rgb(100, 100, 100) }", "<ul><li id=t>x</li></ul>"));
  t.at(0);
  t.flush("#t", "::marker");
  t.set_class("#t", "to");
  t.flush("#t", "::marker");
  EXPECT_EQ(t.width("#t", "::marker", "color"), "rgb(50, 50, 50)");
}

/* "Note that this means that computed values resulting from CSS transitions can inherit to descendants just like any other computed values." */
TEST(CssomTransition, ATransitionOfAnInheritedPropertyIsInherited) {
  Tr t(page("#p { font-size: 10px; transition: font-size 1s linear } #p.big { font-size: 30px }", "<div id=p><span id=t></span></div>"));
  t.at(0);
  t.flush("#t");
  t.set_class("#p", "big");
  t.flush("#t");
  t.at(500);
  EXPECT_EQ(t.width("#p", nullptr, "font-size"), "20px");
  EXPECT_EQ(t.width("#t", nullptr, "font-size"), "20px") << "the child inherits the transitioning value of its parent";
}

/* "If an element is no longer in the document, implementations must cancel any running transitions on it". */
TEST(CssomTransition, ForgettingAnElementCancelsItsTransitions) {
  Tr t(page(kBox));
  t.at(0);
  t.flush("#t");
  t.set_class("#t", "on");
  t.flush("#t");
  t.at(500);
  EXPECT_EQ(t.width("#t"), "150px");
  EXPECT_EQ(gtext_cssom_transition_tracker_forget(t.tracker, t.m.handle("#t")), 1u);
  EXPECT_EQ(t.width("#t"), "200px");
  EXPECT_EQ(gtext_cssom_transition_tracker_forget(t.tracker, t.m.handle("#t")), 0u);
}

/* A reversal half way: the transition back is shortened to the distance travelled (Transitions 1, "Faster reversing of interrupted transitions"). */
TEST(CssomTransition, ReversingAHoverTakesTheDistanceTravelled) {
  Tr t(page(kBox));
  t.at(0);
  t.flush("#t");
  t.set_class("#t", "on");
  t.flush("#t");
  t.at(250);
  t.set_class("#t", "");
  t.flush("#t");
  EXPECT_EQ(t.width("#t"), "125px");
  t.at(375);
  EXPECT_EQ(t.width("#t"), "112.5px") << "a quarter of the distance, in a quarter of the time";
  t.at(500);
  EXPECT_EQ(t.width("#t"), "100px");
}

/* The after-change style is the style without transitions, whatever the layout says. */
TEST(CssomTransition, TheAfterChangeStyleHasNoTransitions) {
  Tr t(page(kBox));
  t.at(0);
  t.flush("#t");
  t.set_class("#t", "on");
  t.flush("#t");
  t.at(500);
  GTEXT_CSS_Computed_Style * after = nullptr;
  ASSERT_EQ(gtext_cssom_document_after_change_style(t.m.doc->d, t.m.handle("#t"), nullptr, 0, &after), GTEXT_CSS_OK);
  ASSERT_NE(after, nullptr);
  char * text = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_serialize_value_buffer(gtext_css_computed_root(after, gtext_css_registry_find_property(R(), "width", 5)), nullptr, &text, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(text, n), "200px");
  gtext_css_serialize_buffer_free(nullptr, text);
  gtext_css_computed_free(after);
  GTEXT_CSS_Computed_Style * none = nullptr;
  EXPECT_EQ(gtext_cssom_document_after_change_style(t.m.doc->d, t.m.handle("#t"), "::nonsense", 10, &none), GTEXT_CSS_OK);
  EXPECT_EQ(none, nullptr);
  EXPECT_EQ(gtext_cssom_document_after_change_style(nullptr, t.m.handle("#t"), nullptr, 0, &none), GTEXT_CSS_E_INVALID);
}

/* A layout that gives transitions and no version keeps no style across reads: the answer follows the tracker's time at every read. */
TEST(CssomTransition, WithoutAVersionNoStyleIsKept) {
  Tr t(page(kBox), true, nullptr, false);
  t.at(0);
  t.flush("#t");
  t.set_class("#t", "on");
  t.flush("#t");
  EXPECT_EQ(t.width("#t"), "100px");
  t.at(500);
  EXPECT_EQ(t.width("#t"), "150px");
}

/* The same for a live style object kept across the reads: it refreshes itself at each read (a style kept for the object would be valid again, the tree being unchanged), and with no version
   the time is not part of what it was valid for. */
TEST(CssomTransition, WithoutAVersionALiveStyleObjectIsNotValidAcrossAMoveOfTheTime) {
  Tr t(page(kBox), true, nullptr, false);
  t.at(0);
  t.flush("#t");
  t.set_class("#t", "on");
  t.flush("#t");
  Decl d = t.m.computed("#t");
  ASSERT_NE(d.d, nullptr);
  EXPECT_EQ(get(d.d, "width"), "100px");
  t.at(500);
  EXPECT_EQ(get(d.d, "width"), "150px");
  t.at(1000);
  EXPECT_EQ(get(d.d, "width"), "200px");
}


/* A container query that needs a size the layout did not give cannot be answered: the read says so (GTEXT_CSS_E_UNSUPPORTED), and a style change event before the read does not turn that
   into an answer. */
TEST(CssomTransition, AQueryThatNeedsALayoutStillFailsTheReadAfterAnEvent) {
  const std::string css = "#p { container-type: size } @container (min-width: 10px) { #t { color: rgb(255, 0, 0) } } #t { color: rgb(0, 128, 0) }";
  Tr t(page(css, "<div id=p><div id=t></div></div>"));
  GTEXT_CSS_Status before = GTEXT_CSS_OK;
  {
    Decl d = t.m.computed("#t", nullptr, &before);
    std::string v = d.d ? get(d.d, "color") : "<no declaration>";
    (void)v;
    GTEXT_CSS_Status read = GTEXT_CSS_OK;
    char * out = nullptr;
    size_t n = 0;
    read = gtext_cssom_decl_get_property_value(d.d, "color", 5, &out, &n);
    before = read;
    if (read == GTEXT_CSS_OK) {
      gtext_cssom_buffer_free(gtext_cssom_document_allocator(t.m.doc->d), out);
    }
  }
  t.at(0);
  const GTEXT_CSS_Status flushed = gtext_cssom_transition_tracker_flush(t.tracker, t.m.handle("#t"), nullptr, 0);
  GTEXT_CSS_Status after = GTEXT_CSS_OK;
  {
    Decl d = t.m.computed("#t");
    char * out = nullptr;
    size_t n = 0;
    after = gtext_cssom_decl_get_property_value(d.d, "color", 5, &out, &n);
    if (after == GTEXT_CSS_OK) {
      gtext_cssom_buffer_free(gtext_cssom_document_allocator(t.m.doc->d), out);
    }
  }
  EXPECT_EQ(after, before) << "flush status " << flushed;
}

/* ---- allocation failure ---------------------------------------------------------------------------------------------------------------------------- */

/* Every allocation of a style change event (the tracker's entries and sets, the after-change styles, the update) and of the read that shows the transition fails in turn: each call is
   what it would have been or is out of memory, the tracker is consistent afterwards (the calls made again succeed and show the transition), and nothing is held at the end. */
TEST(CssomTransitionOom, EveryAllocationOfAnEventAndAReadFailsCleanly) {
  size_t failures = 0;
  for (size_t n = 1; n < 100000; n++) {
    FaultAllocator fa;
    bool done = false;
    {
      Tr t(page(kBox, "<div id=p><div id=t></div></div>"), true, &fa.alloc);
      t.at(0);
      ASSERT_EQ(gtext_cssom_transition_tracker_flush(t.tracker, t.m.handle("#t"), nullptr, 0), GTEXT_CSS_OK);
      t.set_class("#t", "on");
      fa.arm(n);
      GTEXT_CSS_Status st = gtext_cssom_transition_tracker_flush(t.tracker, t.m.handle("#t"), nullptr, 0);
      const bool flushed = !fa.failed;
      GTEXT_CSS_Status read = GTEXT_CSS_OK;
      std::string shown;
      if (st == GTEXT_CSS_OK) {
        t.at(500);
        GTEXT_CSSOM_Decl * d = nullptr;
        read = gtext_cssom_computed_style(t.m.doc->d, t.m.handle("#t"), nullptr, 0, &d);
        if (read == GTEXT_CSS_OK) {
          Decl dd(d);
          shown = get(dd.d, "width");
          read = fa.failed ? GTEXT_CSS_E_OOM : GTEXT_CSS_OK;
        }
      }
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK) << n;
        EXPECT_EQ(shown, "150px") << n;
        done = true;
      }
      else {
        failures++;
        EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_OOM) << n << ": " << st;
        (void)flushed;
        (void)read;
        /* a failed event is made again, and the transition shows */
        t.at(0);
        EXPECT_EQ(gtext_cssom_transition_tracker_flush(t.tracker, t.m.handle("#t"), nullptr, 0), GTEXT_CSS_OK) << n;
        t.at(500);
        EXPECT_EQ(t.width("#t"), "150px") << n;
      }
    }
    EXPECT_EQ(fa.live.size(), 0u) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
    if (done || ::testing::Test::HasFailure()) {
      break;
    }
  }
  EXPECT_GT(failures, 30u);
}

/* "Four distinct ::scroll-button() pseudo-elements can exist on a scroll container, each associated with a flow-relative direction, based on their originating element's writing mode" (CSS
   Overflow 5): `::scroll-button(up)` and `::scroll-button(block-start)` are one pseudo-element in a horizontal writing mode, so a transition started through either is the one transition, and
   `down` is another. The key of the tracker's set is the button, in the flow of the originating element. */
TEST(CssomTransition, TheButtonsOfAScrollContainerAreKeyedByTheButtonAndNotByTheSpellingOfTheArgument) {
  Tr t(page("#t { overflow: auto; height: 50px } #t::scroll-button(*) { opacity: 0; transition: opacity 1s linear } #t.on::scroll-button(*) { opacity: 1 }"));
  t.at(0);
  t.flush("#t");
  t.flush("#t", "::scroll-button(block-start)");
  t.set_class("#t", "on");
  t.flush("#t");
  t.flush("#t", "::scroll-button(block-start)");
  t.at(500);
  EXPECT_EQ(t.width("#t", "::scroll-button(block-start)", "opacity"), "0.5");
  EXPECT_EQ(t.width("#t", "::scroll-button(up)", "opacity"), "0.5") << "up is the block-start button of a horizontal-tb scroll container: the same pseudo-element";
  EXPECT_EQ(t.width("#t", "::scroll-button(UP)", "opacity"), "0.5") << "the argument is not case-sensitive";
  EXPECT_EQ(t.width("#t", "::scroll-button(down)", "opacity"), "1") << "down is the block-end button: another one, which nothing transitioned";
  EXPECT_EQ(t.width("#t", "::scroll-button(inline-start)", "opacity"), "1");
  t.at(1000);
  EXPECT_EQ(t.width("#t", "::scroll-button(up)", "opacity"), "1");
}

/* ... and the button an argument names is the flow of the originating element's: in a vertical writing mode `up` is the inline-start button, and the block-start one is `right` or `left`. */
TEST(CssomTransition, WhichButtonUpIsFollowsTheWritingModeOfTheContainer) {
  Tr t(page("#t { overflow: auto; height: 50px; writing-mode: vertical-rl } #t::scroll-button(*) { opacity: 0; transition: opacity 1s linear } #t.on::scroll-button(*) { opacity: 1 }"));
  t.at(0);
  t.flush("#t");
  t.flush("#t", "::scroll-button(inline-start)");
  t.set_class("#t", "on");
  t.flush("#t");
  t.flush("#t", "::scroll-button(inline-start)");
  t.at(500);
  EXPECT_EQ(t.width("#t", "::scroll-button(inline-start)", "opacity"), "0.5");
  EXPECT_EQ(t.width("#t", "::scroll-button(up)", "opacity"), "0.5") << "up is the inline-start button of a vertical-rl container";
  EXPECT_EQ(t.width("#t", "::scroll-button(block-start)", "opacity"), "1") << "block-start is right here: another button";
  EXPECT_EQ(t.width("#t", "::scroll-button(right)", "opacity"), "1");
}

} // namespace
