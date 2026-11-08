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
 * libFuzzer harness for CSS Transitions (css_transition.h).
 *
 * The input is a script of style change events: each picks a declaration list from a pool as the after-change style (the before-change style is the previous event's), a time, and whether
 * to compute the style that is shown from the set. The properties it holds, whatever the script:
 *
 *  - an update answers OK, and a style computed with the set answers OK;
 *  - "This specification maintains the invariant that there is never both a running transition and a completed transition for the same property and element";
 *  - a running transition has an end time not before its start time, and a shortening factor in 0..1;
 *  - the style shown at the time of an event after it ran is a style, and a style computed from the same set at the same time twice is the same (a pure function of its inputs);
 *  - a failed allocation (every fourth input arms one) is OOM or OK, leaves the set as it was, and leaks nothing.
 *
 * Build with: make fuzz-css-transition      Run: make fuzz-run-css-transition
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include <ghoti.io/text/css.h>

#include "../css_svg_registry.h"
#include "../markup_test_support.h"

namespace {

[[noreturn]] void fail(const char * what, const std::string & detail) {
  std::fprintf(stderr, "css-transition fuzz: %s %s\n", what, detail.c_str());
  std::abort();
}

const char * const kBase[] = {
  "transition: opacity 1s linear; ",
  "transition: opacity 2s ease-in 500ms, width 1s steps(3), margin 3s -1s linear; ",
  "transition: all 1s cubic-bezier(0, 2, 1, 2); ",
  "transition: all 1s cubic-bezier(0, -1, 1, -1) -250ms; ",
  "transition: color 1s; transition-behavior: allow-discrete; ",
  "transition: visibility 1s linear; transition-behavior: allow-discrete; ",
  "transition-property: opacity, width; transition-duration: 1s; ",
  "transition: none; ",
  "transition: opacity 0s; ",
  "transition: margin-inline-start 1s linear; ",
  "transition: font-size 1s linear; ",
  "",
};
const char * const kValues[] = {
  "opacity: 0",       "opacity: 1",         "opacity: 0.5",       "width: 0px",         "width: 100px",       "width: 50%",
  "color: red",       "color: rgb(0, 0, 255)", "visibility: hidden", "visibility: visible", "margin-left: 10px",   "margin-top: 5px",
  "font-size: 10px",  "font-size: 30px",    "width: calc(10px + 5%)", "opacity: 1 !important", "color: currentcolor", "margin: 0",
};

GTEXT_CSS_Compute_Context * context() {
  static GTEXT_CSS_Compute_Context * c = nullptr;
  if (!c) {
    GTEXT_CSS_Environment env = gtext_css_environment_default();
    GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
    GTEXT_CSS_Compute_Result r{};
    if (gtext_css_compute_context_new(svgreg::shared(), &env, &o, &c, &r) != GTEXT_CSS_OK) {
      fail("a context could not be made", r.name);
    }
  }
  return c;
}

GTEXT_CSS_Status style_of(const std::string & decls, const GTEXT_CSS_Transition_Set * set, double t, const GTEXT_CSS_Computed_Style * parent, GTEXT_CSS_Computed_Style ** out) {
  GTEXT_CSS_Style_Transitions tr{};
  tr.size = sizeof tr;
  tr.set = set;
  tr.time_ms = t;
  GTEXT_CSS_Style_Options o{};
  o.transitions = set ? &tr : nullptr;
  GTEXT_CSS_Style_Entry e;
  std::memset(&e, 0, sizeof e);
  e.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  e.text = {decls.data(), decls.size()};
  return gtext_css_style_compute(context(), nullptr, parent, &e, 1, &o, out, nullptr);
}

std::string dump(const GTEXT_CSS_Computed_Style * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
    static_cast<std::string *>(u)->append(b, n);
    return GTEXT_MARKUP_OK;
  };
  if (gtext_css_computed_dump(s, &sink) != GTEXT_CSS_OK) {
    return "<dump failed>";
  }
  return out;
}

void check_set(const GTEXT_CSS_Transition_Set * set) {
  std::set<GTEXT_CSS_Property_Id> running;
  for (size_t i = 0; i < gtext_css_transition_set_running_count(set); i++) {
    GTEXT_CSS_Transition_Info t{};
    if (!gtext_css_transition_set_running_at(set, i, &t)) {
      fail("a running transition could not be read", "");
    }
    if (t.end_time_ms < t.start_time_ms) {
      fail("a running transition ends before it starts", std::to_string(t.start_time_ms) + " " + std::to_string(t.end_time_ms));
    }
    if (!(t.reversing_shortening_factor >= 0 && t.reversing_shortening_factor <= 1)) {
      fail("a shortening factor is outside 0..1", std::to_string(t.reversing_shortening_factor));
    }
    running.insert(t.property);
  }
  for (size_t i = 0; i < gtext_css_transition_set_completed_count(set); i++) {
    GTEXT_CSS_Property_Id p = 0;
    if (!gtext_css_transition_set_completed_at(set, i, &p, nullptr)) {
      fail("a completed transition could not be read", "");
    }
    if (running.count(p)) {
      fail("a property is both running and completed", std::to_string(p));
    }
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  FaultAllocator fa;
  const bool oom = (data[0] & 3) == 0;
  GTEXT_CSS_Transition_Set * set = nullptr;
  if (gtext_css_transition_set_new(&fa.alloc, &set) != GTEXT_CSS_OK) {
    return 0;
  }
  std::string base = kBase[data[1] % (sizeof kBase / sizeof *kBase)];
  GTEXT_CSS_Computed_Style * before = nullptr;
  double time = 0;
  for (size_t i = 2; i + 2 < size && i < 2 + 3 * 12; i += 3) {
    if (data[i] % 5 == 0) {
      base = kBase[data[i + 1] % (sizeof kBase / sizeof *kBase)];
    }
    std::string decls = base + kValues[data[i] % (sizeof kValues / sizeof *kValues)] + "; " + kValues[data[i + 1] % (sizeof kValues / sizeof *kValues)];
    time += (data[i + 2] % 16) * 125.0 - (data[i + 2] == 255 ? 3000.0 : 0.0);
    GTEXT_CSS_Computed_Style * after = nullptr;
    if (style_of(decls, nullptr, time, nullptr, &after) != GTEXT_CSS_OK || !after) {
      fail("a style without transitions could not be computed", decls);
    }
    const uint64_t version = gtext_css_transition_set_version(set);
    const size_t running = gtext_css_transition_set_running_count(set);
    const size_t held = fa.live.size();
    if (oom) {
      fa.arm(1 + data[i + 2] % 40);
    }
    const GTEXT_CSS_Status st = gtext_css_transitions_update(context(), set, before, after, time, nullptr);
    fa.disarm();
    if (st == GTEXT_CSS_E_OOM) {
      if (gtext_css_transition_set_version(set) != version || gtext_css_transition_set_running_count(set) != running || fa.live.size() != held) {
        fail("a failed update changed the set or leaked", decls);
      }
    }
    else if (st != GTEXT_CSS_OK) {
      fail("an update failed with a status other than OOM", std::to_string(static_cast<int>(st)));
    }
    check_set(set);
    GTEXT_CSS_Computed_Style * shown = nullptr;
    GTEXT_CSS_Computed_Style * again = nullptr;
    if (style_of(decls, set, time, nullptr, &shown) != GTEXT_CSS_OK || style_of(decls, set, time, nullptr, &again) != GTEXT_CSS_OK) {
      fail("a style with transitions could not be computed", decls);
    }
    if (dump(shown) != dump(again)) {
      fail("the same set at the same time gave two styles", decls);
    }
    GTEXT_CSS_Computed_Style * child = nullptr;
    if (style_of("", set, time, shown, &child) != GTEXT_CSS_OK) {
      fail("the child of a transitioning element could not be computed", decls);
    }
    gtext_css_computed_free(child);
    gtext_css_computed_free(shown);
    gtext_css_computed_free(again);
    gtext_css_computed_free(before);
    before = after;
  }
  gtext_css_computed_free(before);
  gtext_css_transition_set_free(set);
  if (!fa.live.empty() || fa.bad_frees) {
    fail("a block is left or freed twice", std::to_string(fa.live.size()));
  }
  return 0;
}
