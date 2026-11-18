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
 * The readings this library chose where the text it implements and the other things that say what is right (the pages of the web-platform-tests, the answers of the browsers)
 * do not agree. Each is one constant here and one place of the code that reads it, so that choosing the other reading is a change of one line and of the tests that hold the
 * reading (each names its test), and never a search. They are compile-time constants: the library has no option for them, because the text has no such option.
 *
 * Changing one means changing the tests that hold it too, and the lines of the conformance gates' exception lists that exist because of it (each of those lines says so in its
 * reason, and a gate fails on an exception that no longer holds, which is how a forgotten one is found).
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_READINGS_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_READINGS_H

#include <ghoti.io/text/macros.h>

/**
 * `color-mix()` whose omitted percentages make the sum of the specified ones fall short of 100%, as in `color-mix(in srgb, red 60%, green, blue 50%)`.
 *
 * 1 clamps the share of an omitted percentage at 0% when it is written (`green 0%`), which is what the normalization of "normalize mix percentages" (Values 5) and the pages of the
 * web-platform-tests expect. 0 writes what "serialized percentages" (Color 5) says, `(100% - the specified sum) / the number of omitted percentages`, which is negative here
 * (`green -10%`) and is what Firefox writes (Chromium does not accept the form). Used by ser_mix() in css_color_serialize.c; held by the test
 * CssColorMix.AnOmittedPercentageIsWrittenAsTheSectionSaysEvenWhenItIsNegative.
 */
#define GTEXT_CSS_READING_MIX_OMITTED_PERCENT_CLAMPED 0

/**
 * `param()` of Link Parameters 1 with no colon: `param(--a)`.
 *
 * 1 is valid ("param() = param( <param-spec> [ ':' <declaration-value>? ]? )", and "param(--foo) indicates that the link parameter is using the default value"), which is what the text
 * says. 0 requires the colon, which is what the page css/css-link-params/link-parameters-invalid.html expects (and no browser has link parameters). Used by the matcher of a
 * `<param-spec>` and by the check of the modifiers of `url()` (css_grammar_match.c); held by the `link-parameters: param(--a)`
 * line of tests/test-css-computed-completion.cpp and by the five `param(` lines of tools/conformance/CSS_PROPERTY_EXCEPTIONS.
 */
#define GTEXT_CSS_READING_PARAM_COLON_OPTIONAL 1

/**
 * `env()` whose first argument is not `<custom-ident> <integer [0,∞]>*`, as in `env(10px)`, `env(test 0.1, green)` or `env(safe-area-inset-top ())`.
 *
 * 1 is valid when the declaration is read, and takes its fallback (or is invalid at computed-value time) when `env()` is replaced: css-env-1's `<env-args> = env( <declaration-value>,
 * <declaration-value>? )`, where a name that is not recognized is not an error of syntax. 0 refuses it when the declaration is read, which is what both browsers do
 * (Chromium 156.0.8078.47 and Firefox 158.0.1, a probe of 33 forms) and what their stylesheets' text round trip therefore shows. Used by css_substitution_syntax() in css_value.c; held by
 * the forms of `env()` with no name in tests/test-css-property.cpp and by the `browsers-differ` lines of tools/conformance/CSS_PROPERTY_EXCEPTIONS.
 */
#define GTEXT_CSS_READING_ENV_UNKNOWN_NAME_VALID 1

/**
 * An unquoted font family name of several identifiers, as in `Noto Color Emoji` or `Foo serif`.
 *
 * Fonts 4, 2.1.1 says "Any identifier which could be misinterpreted as a pre-defined keyword in the font-family value definition, or the CSS-wide keywords, is not allowed", and
 * its rule for a keyword is that a name that is "the same as" one must be quoted. 0 applies the sentence to every identifier of the name, so `Noto Color Emoji` is invalid
 * (it holds `emoji`). 1 applies it to the first identifier only for the generic family words (a CSS-wide keyword is refused wherever it stands, as the at-rule tests of both browsers' descriptors hold), which is what Chrome 157.0.8103.78 answers (`CSS.supports`: `Foo emoji`, `Foo serif`, `Foo math`, and `Foo system-ui`
 * are valid, `serif Foo` is not) and what real fonts need (Noto Color Emoji, Noto Sans Math, STIX Two Math). Used by the matcher of `<font-family-custom-ident>` (css_grammar_match.c) and by
 * the family names of the at-rules (css_atrule.c); held by the CssProperty and CssAtRule tests named AFamilyName...
 */
#define GTEXT_CSS_READING_FAMILY_KEYWORD_ONLY_FIRST 1

/**
 * Whether an element with `display: contents` is "being rendered" for CSS Transitions: starting a transition on it, keeping a running one when the element becomes `contents`, and
 * holding the before-change style of one that is `contents`.
 *
 * Transitions 2 uses the HTML term "being rendered" for the elements that start transitions and for those whose running transitions are cancelled, and the text this library is given does
 * not define it for `display: contents`; the one sentence it has about `contents` (Web Animations 1, "commit computed styles") says "the definition of being rendered with regards to
 * `display: contents` is still under discussion" and for that procedure assumes that a connected `display: contents` element not below a `display: none` one is being rendered. 1 follows
 * that assumption: the element starts transitions and keeps them (a transition on it, a CSS animation on it, and the page css/css-animations/display-contents-animates.html agree, and Chromium
 * 156.0.8078.47 keeps a running transition and animation when the element becomes `contents`). 0 treats `contents` as not rendered, as `none` is: nothing starts and what runs is cancelled
 * at the style change event, which is what Firefox 158.0.1 does to a running transition and a running CSS animation when the element becomes `contents` (it starts one on a `contents`
 * element and keeps a script animation, so its answer is not one rule). Undecidable by the text and split by the browsers; the reading is the text's assumption. Used by the check of the tracker's
 * style change events (cssom_computed.c, style_is_not_rendered()); held by the test CssomTransitionEvents.ADisplayContentsElementIsRendered.
 */
#define GTEXT_CSS_READING_DISPLAY_CONTENTS_RENDERED 1

#endif
