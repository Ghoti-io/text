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

#ifndef GHOTI_TEXT_CSS_READINGS_H
#define GHOTI_TEXT_CSS_READINGS_H

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

#endif
