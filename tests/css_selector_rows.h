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
 * One probe for every selector the tables implement (tools/css/selectors.tsv): the name as webref spells it, a selector that uses it, what its serialization is
 * and its specificity. tests/test-css-selector.cpp parses, writes and counts every probe, and `make check-css-selectors` fails on an implemented or parse-only row
 * that has no probe here.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_SELECTOR_ROWS_H
#define GHOTI_IO_GTEXT_TESTS_CSS_SELECTOR_ROWS_H

struct SelectorProbe {
  const char * name;
  const char * probe;
  const char * serialized;
  unsigned a, b, c;
};

static const SelectorProbe selector_probes[] = {
  {" ", "a b", "a b", 0, 0, 2},
  {"+", "a + b", "a + b", 0, 0, 2},
  {">", "a > b", "a > b", 0, 0, 2},
  {"||", "a || b", "a || b", 0, 0, 2},
  {"~", "a ~ b", "a ~ b", 0, 0, 2},
  {"*", "*", "*", 0, 0, 0},
  {".class", ".a", ".a", 0, 1, 0},
  {"E", "div", "div", 0, 0, 1},
  {"[attr]", "[a=\"b\" i]", "[a=\"b\" i]", 0, 1, 0},
  {"ns|", "|a", "|a", 0, 0, 1},
  {"&", "&", "&", 0, 1, 0},
  {":active", ":active", ":active", 0, 1, 0},
  {":active-view-transition", ":active-view-transition", ":active-view-transition", 0, 1, 0},
  {":active-view-transition-type()", ":active-view-transition-type(a, b)", ":active-view-transition-type(a, b)", 0, 1, 0},
  {":animated-image", ":animated-image", ":animated-image", 0, 1, 0},
  {":any-link", ":any-link", ":any-link", 0, 1, 0},
  {":autofill", ":autofill", ":autofill", 0, 1, 0},
  {":blank", ":blank", ":blank", 0, 1, 0},
  {":buffering", ":buffering", ":buffering", 0, 1, 0},
  {":checked", ":checked", ":checked", 0, 1, 0},
  {":current", ":current", ":current", 0, 1, 0},
  {":current()", ":current(.a, .b)", ":current(.a, .b)", 0, 1, 0},
  {":default", ":default", ":default", 0, 1, 0},
  {":defined", ":defined", ":defined", 0, 1, 0},
  {":dir()", ":dir(rtl)", ":dir(rtl)", 0, 1, 0},
  {":disabled", ":disabled", ":disabled", 0, 1, 0},
  {":empty", ":empty", ":empty", 0, 1, 0},
  {":enabled", ":enabled", ":enabled", 0, 1, 0},
  {":first-child", ":first-child", ":first-child", 0, 1, 0},
  {":first-of-type", ":first-of-type", ":first-of-type", 0, 1, 0},
  {":focus", ":focus", ":focus", 0, 1, 0},
  {":focus-visible", ":focus-visible", ":focus-visible", 0, 1, 0},
  {":focus-within", ":focus-within", ":focus-within", 0, 1, 0},
  {":fullscreen", ":fullscreen", ":fullscreen", 0, 1, 0},
  {":future", ":future", ":future", 0, 1, 0},
  {":has()", ":has(> .a, + #b)", ":has(> .a, + #b)", 1, 0, 0},
  {":has-slotted", ":has-slotted", ":has-slotted", 0, 1, 0},
  {":heading", ":heading", ":heading", 0, 1, 0},
  {":heading()", ":heading(1, 2)", ":heading(1, 2)", 0, 1, 0},
  {":high-value", ":high-value", ":high-value", 0, 1, 0},
  {":host", ":host", ":host", 0, 1, 0},
  {":host()", ":host(.a)", ":host(.a)", 0, 2, 0},
  {":host-context()", ":host-context(.a)", ":host-context(.a)", 0, 2, 0},
  {":hover", ":hover", ":hover", 0, 1, 0},
  {":in-range", ":in-range", ":in-range", 0, 1, 0},
  {":indeterminate", ":indeterminate", ":indeterminate", 0, 1, 0},
  {":interest-source", ":interest-source", ":interest-source", 0, 1, 0},
  {":interest-target", ":interest-target", ":interest-target", 0, 1, 0},
  {":invalid", ":invalid", ":invalid", 0, 1, 0},
  {":is()", ":is(.a, #b)", ":is(.a, #b)", 1, 0, 0},
  {":lang()", ":lang(de, \"fr-*\")", ":lang(de, \"fr-*\")", 0, 1, 0},
  {":last-child", ":last-child", ":last-child", 0, 1, 0},
  {":last-of-type", ":last-of-type", ":last-of-type", 0, 1, 0},
  {":link", ":link", ":link", 0, 1, 0},
  {":local-link", ":local-link", ":local-link", 0, 1, 0},
  {":low-value", ":low-value", ":low-value", 0, 1, 0},
  {":matches()", ":matches(.a, #b)", ":is(.a, #b)", 1, 0, 0},
  {":modal", ":modal", ":modal", 0, 1, 0},
  {":muted", ":muted", ":muted", 0, 1, 0},
  {":navigation-source", ":navigation-source", ":navigation-source", 0, 1, 0},
  {":not()", ":not(.a, #b)", ":not(.a, #b)", 1, 0, 0},
  {":nth-child()", ":nth-child(2n+1 of .a)", ":nth-child(2n+1 of .a)", 0, 2, 0},
  {":nth-col()", ":nth-col(2)", ":nth-col(2)", 0, 1, 0},
  {":nth-last-child()", ":nth-last-child(odd)", ":nth-last-child(2n+1)", 0, 1, 0},
  {":nth-last-col()", ":nth-last-col(2)", ":nth-last-col(2)", 0, 1, 0},
  {":nth-last-of-type()", ":nth-last-of-type(-n+2)", ":nth-last-of-type(-n+2)", 0, 1, 0},
  {":nth-of-type()", ":nth-of-type(3)", ":nth-of-type(3)", 0, 1, 0},
  {":only-child", ":only-child", ":only-child", 0, 1, 0},
  {":only-of-type", ":only-of-type", ":only-of-type", 0, 1, 0},
  {":open", ":open", ":open", 0, 1, 0},
  {":optimal-value", ":optimal-value", ":optimal-value", 0, 1, 0},
  {":optional", ":optional", ":optional", 0, 1, 0},
  {":out-of-range", ":out-of-range", ":out-of-range", 0, 1, 0},
  {":past", ":past", ":past", 0, 1, 0},
  {":paused", ":paused", ":paused", 0, 1, 0},
  {":picture-in-picture", ":picture-in-picture", ":picture-in-picture", 0, 1, 0},
  {":placeholder-shown", ":placeholder-shown", ":placeholder-shown", 0, 1, 0},
  {":playing", ":playing", ":playing", 0, 1, 0},
  {":popover-open", ":popover-open", ":popover-open", 0, 1, 0},
  {":read-only", ":read-only", ":read-only", 0, 1, 0},
  {":read-write", ":read-write", ":read-write", 0, 1, 0},
  {":required", ":required", ":required", 0, 1, 0},
  {":root", ":root", ":root", 0, 1, 0},
  {":scope", ":scope", ":scope", 0, 1, 0},
  {":seeking", ":seeking", ":seeking", 0, 1, 0},
  {":snapped", ":snapped", ":snapped", 0, 1, 0},
  {":snapped-block", ":snapped-block", ":snapped-block", 0, 1, 0},
  {":snapped-inline", ":snapped-inline", ":snapped-inline", 0, 1, 0},
  {":snapped-x", ":snapped-x", ":snapped-x", 0, 1, 0},
  {":snapped-y", ":snapped-y", ":snapped-y", 0, 1, 0},
  {":stalled", ":stalled", ":stalled", 0, 1, 0},
  {":state()", ":state(--foo)", ":state(--foo)", 0, 1, 0},
  {":target", ":target", ":target", 0, 1, 0},
  {":target-after", ":target-after", ":target-after", 0, 1, 0},
  {":target-before", ":target-before", ":target-before", 0, 1, 0},
  {":target-current", ":target-current", ":target-current", 0, 1, 0},
  {":target-within", ":target-within", ":target-within", 0, 1, 0},
  {":unchecked", ":unchecked", ":unchecked", 0, 1, 0},
  {":user-invalid", ":user-invalid", ":user-invalid", 0, 1, 0},
  {":user-valid", ":user-valid", ":user-valid", 0, 1, 0},
  {":valid", ":valid", ":valid", 0, 1, 0},
  {":visited", ":visited", ":visited", 0, 1, 0},
  {":volume-locked", ":volume-locked", ":volume-locked", 0, 1, 0},
  {":where()", ":where(.a, #b)", ":where(.a, #b)", 0, 0, 0},
  {":xr-overlay", ":xr-overlay", ":xr-overlay", 0, 1, 0},
  {":after", ":after", "::after", 0, 0, 1},
  {"::after", "::after", "::after", 0, 0, 1},
  {"::backdrop", "::backdrop", "::backdrop", 0, 0, 1},
  {":before", ":before", "::before", 0, 0, 1},
  {"::before", "::before", "::before", 0, 0, 1},
  {"::checkmark", "::checkmark", "::checkmark", 0, 0, 1},
  {"::clear-icon", "::clear-icon", "::clear-icon", 0, 0, 1},
  {"::color-swatch", "::color-swatch", "::color-swatch", 0, 0, 1},
  {"::column", "::column", "::column", 0, 0, 1},
  {"::cue", "::cue", "::cue", 0, 0, 1},
  {"::cue()", "::cue(.a)", "::cue(.a)", 0, 0, 1},
  {"::cue-region", "::cue-region", "::cue-region", 0, 0, 1},
  {"::cue-region()", "::cue-region(.a)", "::cue-region(.a)", 0, 0, 1},
  {"::details-content", "::details-content", "::details-content", 0, 0, 1},
  {"::field-component", "::field-component", "::field-component", 0, 0, 1},
  {"::field-content", "::field-content", "::field-content", 0, 0, 1},
  {"::field-separator", "::field-separator", "::field-separator", 0, 0, 1},
  {"::file-selector-button", "::file-selector-button", "::file-selector-button", 0, 0, 1},
  {":first-letter", ":first-letter", "::first-letter", 0, 0, 1},
  {"::first-letter", "::first-letter", "::first-letter", 0, 0, 1},
  {":first-line", ":first-line", "::first-line", 0, 0, 1},
  {"::first-line", "::first-line", "::first-line", 0, 0, 1},
  {"::grammar-error", "::grammar-error", "::grammar-error", 0, 0, 1},
  {"::highlight()", "::highlight(x)", "::highlight(x)", 0, 0, 1},
  {"::interest-button", "::interest-button", "::interest-button", 0, 0, 1},
  {"::marker", "::marker", "::marker", 0, 0, 1},
  {"::nth-fragment()", "::nth-fragment(2n)", "::nth-fragment(2n)", 0, 0, 1},
  {"::part()", "::part(a b)", "::part(a b)", 0, 0, 1},
  {"::picker()", "::picker(select)", "::picker(select)", 0, 0, 1},
  {"::picker-icon", "::picker-icon", "::picker-icon", 0, 0, 1},
  {"::placeholder", "::placeholder", "::placeholder", 0, 0, 1},
  {"::reveal-icon", "::reveal-icon", "::reveal-icon", 0, 0, 1},
  {"::scroll-button()", "::scroll-button(up)", "::scroll-button(up)", 0, 0, 1},
  {"::scroll-marker", "::scroll-marker", "::scroll-marker", 0, 0, 1},
  {"::scroll-marker-group", "::scroll-marker-group", "::scroll-marker-group", 0, 0, 1},
  {"::search-text", "::search-text", "::search-text", 0, 0, 1},
  {"::selection", "::selection", "::selection", 0, 0, 1},
  {"::slider-fill", "::slider-fill", "::slider-fill", 0, 0, 1},
  {"::slider-thumb", "::slider-thumb", "::slider-thumb", 0, 0, 1},
  {"::slider-track", "::slider-track", "::slider-track", 0, 0, 1},
  {"::slotted()", "::slotted(.a)", "::slotted(.a)", 0, 1, 1},
  {"::spelling-error", "::spelling-error", "::spelling-error", 0, 0, 1},
  {"::step-control", "::step-control", "::step-control", 0, 0, 1},
  {"::step-down", "::step-down", "::step-down", 0, 0, 1},
  {"::step-up", "::step-up", "::step-up", 0, 0, 1},
  {"::target-text", "::target-text", "::target-text", 0, 0, 1},
  {"::view-transition", "::view-transition", "::view-transition", 0, 0, 1},
  {"::view-transition-group()", "::view-transition-group(*)", "::view-transition-group(*)", 0, 0, 1},
  {"::view-transition-group-children()", "::view-transition-group-children(*)", "::view-transition-group-children(*)", 0, 0, 1},
  {"::view-transition-image-pair()", "::view-transition-image-pair(*)", "::view-transition-image-pair(*)", 0, 0, 1},
  {"::view-transition-new()", "::view-transition-new(*)", "::view-transition-new(*)", 0, 0, 1},
  {"::view-transition-old()", "::view-transition-old(*)", "::view-transition-old(*)", 0, 0, 1},
};

#endif // GHOTI_IO_GTEXT_TESTS_CSS_SELECTOR_ROWS_H
