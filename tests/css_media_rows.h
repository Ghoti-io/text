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
 * One probe for every media feature the tables implement (tools/css/media-features.tsv): the feature as the pin spells it, a query that is true in the probe
 * environment of tests/test-css-media.cpp, a query that is false in it, and the serialization of the true one. `make check-css-conditions` fails on an
 * implemented row that has no probe here.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_MEDIA_ROWS_H
#define GHOTI_IO_GTEXT_TESTS_CSS_MEDIA_ROWS_H

struct MediaProbe {
  const char * name;
  const char * yes;        ///< True in the probe environment.
  const char * no;         ///< False in it.
  const char * serialized; ///< What `yes` serializes to.
};

static const MediaProbe media_probes[] = {
  {"any-hover", "(any-hover: hover)", "(any-hover: none)", "(any-hover: hover)"},
  {"any-pointer", "(any-pointer: coarse)", "(any-pointer: none)", "(any-pointer: coarse)"},
  {"aspect-ratio", "(aspect-ratio: 8 / 5)", "(aspect-ratio: 1 / 1)", "(aspect-ratio: 8 / 5)"},
  {"color", "(color: 8)", "(color: 10)", "(color: 8)"},
  {"color-gamut", "(color-gamut: p3)", "(color-gamut: rec2020)", "(color-gamut: p3)"},
  {"color-index", "(color-index: 256)", "(color-index: 0)", "(color-index: 256)"},
  {"device-aspect-ratio", "(device-aspect-ratio: 16/9)", "(device-aspect-ratio: 4/3)", "(device-aspect-ratio: 16 / 9)"},
  {"device-height", "(device-height: 1080px)", "(device-height: 1px)", "(device-height: 1080px)"},
  {"device-width", "(device-width: 1920px)", "(device-width: 1px)", "(device-width: 1920px)"},
  {"display-mode", "(display-mode: standalone)", "(display-mode: browser)", "(display-mode: standalone)"},
  {"display-state", "(display-state: maximized)", "(display-state: normal)", "(display-state: maximized)"},
  {"dynamic-range", "(dynamic-range: standard)", "(dynamic-range: high)", "(dynamic-range: standard)"},
  {"environment-blending", "(environment-blending: additive)", "(environment-blending: opaque)", "(environment-blending: additive)"},
  {"forced-colors", "(forced-colors: active)", "(forced-colors: none)", "(forced-colors: active)"},
  {"grid", "(grid: 1)", "(grid: 0)", "(grid: 1)"},
  {"height", "(height: 800px)", "(height: 799px)", "(height: 800px)"},
  {"horizontal-viewport-segments", "(horizontal-viewport-segments: 2)", "(horizontal-viewport-segments: 1)", "(horizontal-viewport-segments: 2)"},
  {"hover", "(hover: none)", "(hover: hover)", "(hover: none)"},
  {"inverted-colors", "(inverted-colors: inverted)", "(inverted-colors: none)", "(inverted-colors: inverted)"},
  {"monochrome", "(monochrome: 0)", "(monochrome: 1)", "(monochrome: 0)"},
  {"nav-controls", "(nav-controls: back)", "(nav-controls: none)", "(nav-controls: back)"},
  {"orientation", "(orientation: landscape)", "(orientation: portrait)", "(orientation: landscape)"},
  {"overflow-block", "(overflow-block: paged)", "(overflow-block: scroll)", "(overflow-block: paged)"},
  {"overflow-inline", "(overflow-inline: scroll)", "(overflow-inline: none)", "(overflow-inline: scroll)"},
  {"pointer", "(pointer: coarse)", "(pointer: fine)", "(pointer: coarse)"},
  {"prefers-color-scheme", "(prefers-color-scheme: dark)", "(prefers-color-scheme: light)", "(prefers-color-scheme: dark)"},
  {"prefers-contrast", "(prefers-contrast: more)", "(prefers-contrast: less)", "(prefers-contrast: more)"},
  {"prefers-reduced-data", "(prefers-reduced-data: reduce)", "(prefers-reduced-data: no-preference)", "(prefers-reduced-data: reduce)"},
  {"prefers-reduced-motion", "(prefers-reduced-motion: reduce)", "(prefers-reduced-motion: no-preference)", "(prefers-reduced-motion: reduce)"},
  {"prefers-reduced-transparency", "(prefers-reduced-transparency: reduce)", "(prefers-reduced-transparency: no-preference)", "(prefers-reduced-transparency: reduce)"},
  {"resizable", "(resizable: true)", "(resizable: false)", "(resizable: true)"},
  {"resolution", "(resolution: 2dppx)", "(resolution: 1dppx)", "(resolution: 2dppx)"},
  {"scan", "(scan: interlace)", "(scan: progressive)", "(scan: interlace)"},
  {"scripting", "(scripting: initial-only)", "(scripting: enabled)", "(scripting: initial-only)"},
  {"shape", "(shape: round)", "(shape: rect)", "(shape: round)"},
  {"ua-color-scheme", "(ua-color-scheme: dark)", "(ua-color-scheme: light)", "(ua-color-scheme: dark)"},
  {"update", "(update: slow)", "(update: fast)", "(update: slow)"},
  {"vertical-viewport-segments", "(vertical-viewport-segments: 3)", "(vertical-viewport-segments: 1)", "(vertical-viewport-segments: 3)"},
  {"video-color-gamut", "(video-color-gamut: srgb)", "(video-color-gamut: p3)", "(video-color-gamut: srgb)"},
  {"video-dynamic-range", "(video-dynamic-range: standard)", "(video-dynamic-range: high)", "(video-dynamic-range: standard)"},
  {"width", "(width: 1280px)", "(width: 1279px)", "(width: 1280px)"},
  {"-webkit-device-pixel-ratio", "(-webkit-device-pixel-ratio: 2)", "(-webkit-device-pixel-ratio: 1)", "(-webkit-device-pixel-ratio: 2)"},
  {"-webkit-transform-3d", "(-webkit-transform-3d: 1)", "(-webkit-transform-3d: 0)", "(-webkit-transform-3d: 1)"},
};

#endif
