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

/**
 * An interpolated color of the legacy sRGB syntax (`rgb()`, `hsl()`, hex, names), as in the middle of a transition from black to white.
 *
 * Color 4 says "Implementations should honor the precision of the component as authored or calculated wherever possible. If this is not possible, the component should be rounded towards
 * +∞" and that the precision of an sRGB component "is not defined in this specification, but must at least be sufficient to round-trip eight bit values": the text allows the calculated
 * `rgb(127.5, 127.5, 127.5)` and a rounded `rgb(128, 128, 128)` alike. 1 rounds each channel to a whole 8-bit value (a half goes up), which is what Chromium 156.0.8078.47 and Firefox 158.0.1
 * answer and what the page css/css-transitions/all-interpolates-same-as-explicit-property.html expects. 0 keeps the calculated value. The alpha is not rounded. Used by color_node() in
 * css_anim_interp.c; held by the test CssAnimationInterp.AnInterpolatedLegacyColorIsRoundedToEightBitChannels.
 */
#define GTEXT_CSS_READING_ANIMATED_SRGB_ROUNDED 1

/**
 * A bare number as a length wherever the grammar of the property of an SVG presentation attribute has one, as in `filter="drop-shadow(blue 4 5 6) blur(2)"`
 * or `clip-path="circle(10 at 5 5)"`. (A bare number as an angle is GTEXT_CSS_READING_ATTRIBUTE_ANGLE_NUMBER, below.)
 *
 * SVG 2, "Attribute syntax": "Replace all instances of <length> in grammar with [<length> | <number>]. Replace all instances of <length-percentage> in grammar with
 * [<length-percentage> | <number>]. Replace all instances of <angle> in grammar with [<angle> | <number>]." (the grammar is the one "given in the attribute definition table's Value column",
 * which for a presentation attribute is the property's), and "The insertion of the <number> symbols allows for unitless length and angles to be used in presentation attribute while
 * disallowing them in corresponding property values." So the allowance is not the top level of the value only (what this library read before the page was pinned, from the sentence of
 * SVG 2, 8.3 about `width` and `height` alone): it is every symbol of the grammar, inside a function's arguments too (a math function's contents are not symbols of the grammar, so
 * `calc(4)` is still a number and not a length). 1 reads it so, in px, for `<length>` and `<length-percentage>`. 0 reads a bare number as a length at the top level of the value
 * only. Chromium 156.0.8078.47 and Firefox 158.0.1 read the lengths of `drop-shadow()` and of the basic shapes of `clip-path`, and in the attribute only; Firefox reads `blur(4)`
 * as the text does and Chromium does not. Used by the property layer's
 * attribute mode (css_property.c, run_match()); held by the tests CssPresentation.ADropShadowTakesBareNumbersAsUserUnits, CssPresentation.EveryLengthOfTheGrammarTakesABareNumber
 * and CssPresentation.ABareNumberIsNoLengthWhereTheTextAndTheBrowsersRefuseIt.
 */
#define GTEXT_CSS_READING_ATTRIBUTE_NUMBER_SYMBOLS 1

/**
 * A bare number as an angle in an SVG presentation attribute, as in `filter="hue-rotate(45)"`.
 *
 * SVG 2, "Attribute syntax": "Replace all instances of <angle> in grammar with [<angle> | <number>]." So the text says a bare number is an angle in degrees, wherever the
 * grammar has an <angle>, as it says for a <length>. Chromium 154 and 156.0.8078.47, Firefox 158.0.1, resvg 0.45.1 and Inkscape 1.4 all refuse `hue-rotate(45)` (the zero is valid
 * everywhere), and the vector corpus (filters/filter-functions/hue-rotate-function-45.svg) expects it dropped: the text loses to four engines and is kept as a reading. 0 refuses
 * a bare number where an angle is. 1 reads it as degrees (the text). Used by the property layer's attribute mode (css_property.c, run_match()); held by the tests
 * CssPresentation.BlurTakesABareNumberAndHueRotateDoesNot and, flipped, by the plant reading-attribute-angle-number-is-read.
 */
#define GTEXT_CSS_READING_ATTRIBUTE_ANGLE_NUMBER 0

/**
 * An SVG presentation attribute that is not a value of its property.
 *
 * SVG 2, "Attribute syntax": "When an attribute fails to parse according to the specified CSS Value Definition Syntax ... the attribute is assumed to have been specified as the given
 * initial value", which "can override values that come from lower priority style sheet rules, such as those from the user agent style sheet". Chromium 154 and 156.0.8078.47, Firefox
 * 158.0.1 and resvg 0.45.1 all ignore such an attribute instead: `<g fill="red"><rect fill="bogus"/></g>` is red, `overflow="invalid"` on a nested `svg` leaves the user agent sheet's
 * value. So the text loses to every engine and is kept as a reading. 0 ignores the invalid attribute (no declaration: the cascaded or inherited value stands); 1 reads it as a declaration
 * of `initial` (the text), for SVG rows only. The option GTEXT_CSS_Presentation_Options::invalid_is_initial takes its default from this. Used by gtext_css_presentation_options_default()
 * (css_presentation.c); held by the tests CssStyleRequirements.ABadAttributeIsDroppedWithADiagnosticAndTheValueIsInherited and
 * CssStyleRequirements.AnInvalidAttributeIsIgnoredAndTheOptionMakesItTheInitialValue (the option's value 1 is tested through it) and, flipped, by the plant
 * the-default-options-read-an-invalid-attribute-as-initial.
 */
#define GTEXT_CSS_READING_ATTRIBUTE_INVALID_IS_INITIAL 0

#endif
