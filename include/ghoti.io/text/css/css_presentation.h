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
 * @file css_presentation.h
 * @brief Presentation attributes: the attributes of an element that set a property, as SVG 2 defines them.
 *
 * Reference: SVG 2 (the Candidate Recommendation of 4 October 2018, pinned in third_party/css/svg2): 6.6 "Presentation attributes" (the table of attributes and the elements they
 * are on; "Presentation attributes contribute to the author level of the cascade, following all other author-level style sheets" and "have specificity 0") and "Attribute syntax"
 * (how an attribute's value is parsed: below); CSS Transforms 1, "Syntax of the SVG transform attribute"; CSS Cascade 5, 6.5
 * "Precedence of Non-CSS Presentational Hints" (the entry is its own origin between the user's and the author's, css_cascade.h). The HTML Standard's
 * presentational hints are the same mechanism, with a table of the caller's.
 *
 * ## The table
 *
 * Which attribute sets which property, on which elements, is **data**: a @ref GTEXT_CSS_Presentation_Table of rows. The library's own is
 * SVG 2's (gtext_css_presentation_table_svg2(), generated from tools/css/svg_presentation.tsv, which quotes the specification's list). A caller
 * who needs another list (HTML's presentational hints, a profile of SVG) passes a table of its own in the options; nothing else changes. A
 * table is plain data and is never copied: it must outlive the options that name it.
 *
 * ## Attribute-only spellings
 *
 * An attribute's value is mostly a CSS value, but SVG gives some attributes spellings that the property does not have. They are options of
 * @ref GTEXT_CSS_Presentation_Options, each on by default in gtext_css_presentation_options_default() and each off in a zeroed struct (decision
 * 11 of the plan: quirks are options, not forks):
 * - `unitless_lengths`: a number is a length in user units (`stroke-width="2"` is 2px), wherever the property's grammar has a `<length>` or a
 *   `<length-percentage>` (an angle too in degrees if GTEXT_CSS_READING_ATTRIBUTE_ANGLE_NUMBER is 1, which it is not: no engine reads one): SVG 2's "Attribute syntax" says "Replace all instances of <length> in grammar with [<length> | <number>]" (and the same for
 *   `<length-percentage>` and `<angle>`), so it reaches the arguments of a function too (`filter="drop-shadow(blue 4 5 6) blur(2)"`,
 *   `clip-path="circle(10 at 5 5)"`) and not a math function's, whose contents are no symbols of the grammar (`calc(4)` is a number). The `style` attribute and a sheet do
 *   not read it (SVG 2: it is what "disallowing them in corresponding property values" says). A property gets this from the parse, not from a list of properties (the HTML
 *   quirk of Values 4, Appendix C has one);
 * - `invalid_is_initial`: an attribute that is not a value of its property is read as the property's initial value: "When an attribute fails to parse according to the
 *   specified CSS Value Definition Syntax ... the attribute is assumed to have been specified as the given initial value", which "can override values that come from lower
 *   priority style sheet rules, such as those from the user agent style sheet" (`overflow="invalid"` on a nested `svg` is `visible`, not the `hidden` of the user agent
 *   sheet) and makes an inherited property the initial value, not the parent's (`fill="invalid"` is black below a red `g`). It is SVG's rule, so it is applied to the rows of
 *   a table that are `svg_namespace_only` (every row of SVG 2's) and to no other: an attribute of HTML's presentational hints that does not parse is ignored, whatever the
 *   option says. Without the option an invalid SVG attribute is dropped, as a declaration is, which is what both Chromium and Firefox do: the text's reading is the default
 *   because it is the pinned text. The initial value is a candidate of its own, so it counts against the limit of candidates of the set;
 * - `svg_transform`: the `transform`, `gradientTransform` and `patternTransform` attributes are written in SVG's transform list syntax, and in no other: CSS Transforms 1
 *   says "the syntax of the transform, patternTransform, gradientTransform attributes differ from the syntax of the 'transform' CSS property" and that for them "there is no
 *   support for additional <transform-function>s defined for the CSS 'transform' property". The syntax is numbers only (a CSS number token: `5.` is not one, and a number
 *   that does not fit a float is not accepted), no units, no `none`, `matrix`, `translate`, `scale`, `rotate` (also with the center), `skewX` and `skewY` written exactly so,
 *   separated by white space or a comma. `translate(10px)` is therefore no value of the attribute, and is invalid as it is in Chromium and Firefox, which agree with the text;
 * - `path_data`: the `d` attribute is path data and is the CSS `<string>` that SVG 2's `d: none | <string>` takes; without it the value is the property's;
 * - `trim_whitespace`: the white space at both ends of the value (space, tab, line feed, carriage return and form feed) is not part of it.
 *
 * ## Errors
 *
 * An attribute that is not a value of its property is **invalid**, and that is a diagnostic: its kind, the attribute, the property, and the offset into the attribute's value of
 * the place that was wrong (plus `base_offset`, so a caller maps it to its document). What the cascade sees of it is the `invalid_is_initial` option's: the property's initial
 * value (SVG 2's rule, the default), or nothing, so that a lower-priority declaration, the inherited value or the initial value stands (a declaration's).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_PRESENTATION_H
#define GHOTI_IO_GTEXT_CSS_CSS_PRESENTATION_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_cascade.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The SVG namespace, which the rows of SVG 2's table are limited to. */
#define GTEXT_CSS_SVG_NAMESPACE "http://www.w3.org/2000/svg"

/** @brief How an attribute's value is spelled. */
typedef enum {
  GTEXT_CSS_PRESENTATION_CSS = 0,   ///< It is the property's value.
  GTEXT_CSS_PRESENTATION_TRANSFORM, ///< A transform list: SVG's syntax (an option, and then no other), else the property's.
  GTEXT_CSS_PRESENTATION_PATH       ///< Path data: a string (an option), else the property's.
} GTEXT_CSS_Presentation_Syntax;

/** @brief One attribute of a table. */
typedef struct {
  const char * attribute;                 ///< The attribute's name, case-sensitive (`patternTransform`).
  const char * property;                  ///< The property it sets: the name of a property of the registry.
  bool svg_namespace_only;                ///< Only on an element in the SVG namespace; false: in any namespace.
  const char * elements;                  ///< NULL: every element. Else the local names it is on, separated by commas (case-sensitive); a list that starts with `!` is every element but those.
  GTEXT_CSS_Presentation_Syntax syntax;   ///< How its value is spelled.
} GTEXT_CSS_Presentation_Row;

/** @brief A table of presentation attributes: plain data, never copied. */
typedef struct {
  const GTEXT_CSS_Presentation_Row * rows; ///< The rows.
  size_t count;                            ///< Their number.
} GTEXT_CSS_Presentation_Table;

/** @brief SVG 2's table (tools/css/svg_presentation.tsv). Static; never NULL. */
GTEXT_API const GTEXT_CSS_Presentation_Table * gtext_css_presentation_table_svg2(void);

/** @brief What the attribute-only spellings are, and the offset of the value. A zeroed struct has none of the spellings and the SVG 2 table. */
typedef struct {
  const GTEXT_CSS_Presentation_Table * table; ///< The table; NULL for gtext_css_presentation_table_svg2().
  bool unitless_lengths;                      ///< A number is a length in user units and an angle in degrees (see the file's description).
  bool invalid_is_initial;                    ///< An attribute that is not a value of its property is the property's initial value, not dropped.
  bool svg_transform;                         ///< Transform attributes are written in SVG's syntax.
  bool path_data;                             ///< The `d` attribute is path data.
  bool trim_whitespace;                       ///< The white space at both ends of a value is not part of it.
  size_t base_offset;                         ///< Added to every offset of a diagnostic: where the attribute's value begins in the caller's document.
} GTEXT_CSS_Presentation_Options;

/** @brief Every spelling on, the SVG 2 table, no base offset: what a browser does with an SVG document. */
GTEXT_API GTEXT_CSS_Presentation_Options gtext_css_presentation_options_default(void);

/**
 * @brief The row for an attribute on an element.
 * @param table The table; NULL for SVG 2's.
 * @param ns The element's namespace URI, or NULL for none.
 * @param ns_length Its length.
 * @param local_name The element's local name.
 * @param local_name_length Its length.
 * @param attribute The attribute's name (no namespace).
 * @param attribute_length Its length.
 * @return The row, owned by the table; NULL if the attribute is not a presentation attribute of the element.
 */
GTEXT_API const GTEXT_CSS_Presentation_Row * gtext_css_presentation_find(const GTEXT_CSS_Presentation_Table * table, const char * ns,
    size_t ns_length, const char * local_name, size_t local_name_length, const char * attribute, size_t attribute_length);

/** @brief Why an attribute was invalid. */
typedef enum {
  GTEXT_CSS_PRESENTATION_OK = 0,           ///< Not invalid.
  GTEXT_CSS_PRESENTATION_INVALID_VALUE,    ///< The value is not one of the property.
  GTEXT_CSS_PRESENTATION_UNKNOWN_PROPERTY, ///< The row names a property the registry does not have.
  GTEXT_CSS_PRESENTATION_UNJUDGEABLE       ///< The property's value cannot be judged by this library (GTEXT_CSS_E_UNSUPPORTED): nothing was added.
} GTEXT_CSS_Presentation_Outcome;

/** @brief What was wrong with an attribute, and where. */
typedef struct {
  GTEXT_CSS_Presentation_Outcome outcome; ///< Why it was invalid; OK when it was not.
  size_t offset;                          ///< The byte of the value that was wrong (a value that is not a value at all: its first), plus `base_offset`.
  size_t value_offset;                    ///< Where the value begins (white space trimmed, with the option), plus `base_offset`.
  size_t value_length;                    ///< Its length.
} GTEXT_CSS_Presentation_Diagnostic;

/**
 * @brief The CSS that an attribute's value stands for: what the property would be declared as.
 * `translate(10 20) rotate(45 5 5)` is `translate(10px, 20px) translate(5px, 5px) rotate(45deg) translate(-5px, -5px)`; `d="M0 0"` is `"M0 0"`;
 * a value of a CSS-valued attribute is itself.
 * @param row The row.
 * @param value The attribute's value; need not be terminated.
 * @param length Its length.
 * @param options NULL for the defaults.
 * @param allocator NULL for the default one.
 * @param out Receives a NUL-terminated string, released with gtext_allocator_free() of the same allocator; written only on success. Not NULL.
 * @param out_length Receives its length; NULL if not wanted.
 * @param error_offset Receives, for GTEXT_CSS_E_INVALID, the offset into `value` of the place that was wrong; NULL if not wanted.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, or a transform attribute that is not in SVG's syntax, and then `*error_offset` says where SVG's syntax failed;
 *   a value in the syntax leaves it at (size_t)-1); GTEXT_CSS_E_OOM. A value of a CSS-valued attribute is returned as it is (this function does not judge CSS).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_presentation_value_text(const GTEXT_CSS_Presentation_Row * row, const char * value, size_t length,
    const GTEXT_CSS_Presentation_Options * options, const GTEXT_Allocator * allocator, char ** out, size_t * out_length, size_t * error_offset);

/**
 * @brief Add a presentation attribute to a candidate set, as the hint it is.
 * The entry that is made is author origin, a presentational hint, specificity zero, never important, unlayered (css_cascade.h); `keys` supplies the
 * order of appearance (`sheet`, `rule`, `declaration`) and the `context`, and everything else in it is ignored. An invalid value is not
 * an error: the call returns GTEXT_CSS_OK, the diagnostic says so, and the entry is a declaration of `initial` (option `invalid_is_initial`) or none.
 * @param set The candidate set. Not NULL; not sealed.
 * @param keys The order keys. Not NULL.
 * @param row The attribute's row. Not NULL.
 * @param value The attribute's value; need not be terminated.
 * @param length Its length.
 * @param options NULL for the defaults.
 * @param diagnostic NULL, or receives what happened (OK in its `outcome` if the attribute was added).
 * @param result NULL, or filled as for gtext_css_candidates_add().
 * @return GTEXT_CSS_OK (added or invalid); GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_CANDIDATES; and the other statuses of
 *   gtext_css_candidates_add().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_candidates_add_presentation(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys,
    const GTEXT_CSS_Presentation_Row * row, const char * value, size_t length, const GTEXT_CSS_Presentation_Options * options,
    GTEXT_CSS_Presentation_Diagnostic * diagnostic, GTEXT_CSS_Cascade_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_PRESENTATION_H
