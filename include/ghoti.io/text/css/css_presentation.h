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
 * Reference: SVG 2, 6.6 "Presentation attributes" (the table of attributes and the elements they are on; "Presentation attributes contribute to the
 * author level of the cascade, followed by all other author-level style sheets" and "have specificity 0"), served 2026-10-26; CSS Cascade 5, 6.5
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
 * - `unitless_lengths`: a number is a length in user units (`stroke-width="2"` is 2px), in every property that takes a `<length>` or a
 *   `<length-percentage>`. A property gets this from the parse, not from a list of properties (the HTML quirk of Values 4, Appendix C has one);
 * - `svg_transform`: the `transform`, `gradientTransform` and `patternTransform` attributes are written in SVG's transform list syntax
 *   (`rotate(45 10 10)`, `translate(10,20)`, numbers without units, commas or white space between the numbers). The attribute is read as that
 *   list first and as the CSS `<transform-list>` after it (SVG 2 says the attribute is a CSS transform, so `translate(10px)` is also one);
 * - `path_data`: the `d` attribute is path data and is the CSS value `path("...")`; without it the value is the property's;
 * - `trim_whitespace`: the white space at both ends of the value (space, tab, line feed, carriage return and form feed) is not part of it.
 *
 * ## Errors
 *
 * An attribute that is not a value of its property is **dropped**, as a declaration is, and the drop is a diagnostic: its kind, the attribute,
 * the property, and the offset into the attribute's value of the place that was wrong (plus `base_offset`, so a caller maps it to its
 * document). The cascade then sees nothing of it: a lower-priority declaration, the inherited value or the initial value stands.
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
  GTEXT_CSS_PRESENTATION_TRANSFORM, ///< A transform list: SVG's syntax (an option), else the property's.
  GTEXT_CSS_PRESENTATION_PATH       ///< Path data: `path("...")` (an option), else the property's.
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
  bool unitless_lengths;                      ///< A number is a length in user units (see the file's description).
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

/** @brief Why an attribute was dropped. */
typedef enum {
  GTEXT_CSS_PRESENTATION_OK = 0,           ///< Not dropped.
  GTEXT_CSS_PRESENTATION_INVALID_VALUE,    ///< The value is not one of the property.
  GTEXT_CSS_PRESENTATION_UNKNOWN_PROPERTY, ///< The row names a property the registry does not have.
  GTEXT_CSS_PRESENTATION_UNJUDGEABLE       ///< The property's value cannot be judged by this library (GTEXT_CSS_E_UNSUPPORTED): nothing was added.
} GTEXT_CSS_Presentation_Outcome;

/** @brief What was wrong with an attribute, and where. */
typedef struct {
  GTEXT_CSS_Presentation_Outcome outcome; ///< Why it was dropped; OK when it was not.
  size_t offset;                          ///< The byte of the value that was wrong (a value that is not a value at all: its first), plus `base_offset`.
  size_t value_offset;                    ///< Where the value begins (white space trimmed, with the option), plus `base_offset`.
  size_t value_length;                    ///< Its length.
} GTEXT_CSS_Presentation_Diagnostic;

/**
 * @brief The CSS that an attribute's value stands for: what the property would be declared as.
 * `translate(10 20) rotate(45 5 5)` is `translate(10px, 20px) translate(5px, 5px) rotate(45deg) translate(-5px, -5px)`; `d="M0 0"` is `path("M0 0")`;
 * a value in the property's own syntax is itself.
 * @param row The row.
 * @param value The attribute's value; need not be terminated.
 * @param length Its length.
 * @param options NULL for the defaults.
 * @param allocator NULL for the default one.
 * @param out Receives a NUL-terminated string, released with gtext_allocator_free() of the same allocator; written only on success. Not NULL.
 * @param out_length Receives its length; NULL if not wanted.
 * @param error_offset Receives, for GTEXT_CSS_E_INVALID, the offset into `value` of the place that was wrong; NULL if not wanted.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_OOM. A transform attribute that is not in SVG's syntax is returned as it is, for the
 *   property's own syntax to judge (this function does not judge CSS), and `*error_offset` says where SVG's syntax failed; a value in the syntax leaves it at (size_t)-1.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_presentation_value_text(const GTEXT_CSS_Presentation_Row * row, const char * value, size_t length,
    const GTEXT_CSS_Presentation_Options * options, const GTEXT_Allocator * allocator, char ** out, size_t * out_length, size_t * error_offset);

/**
 * @brief Add a presentation attribute to a candidate set, as the hint it is.
 * The entry that is made is author origin, a presentational hint, specificity zero, never important, unlayered (css_cascade.h); `keys` supplies the
 * order of appearance (`sheet`, `rule`, `declaration`) and the `context`, and everything else in it is ignored. An invalid value is **dropped**, not
 * an error: the call returns GTEXT_CSS_OK and the diagnostic says so.
 * @param set The candidate set. Not NULL; not sealed.
 * @param keys The order keys. Not NULL.
 * @param row The attribute's row. Not NULL.
 * @param value The attribute's value; need not be terminated.
 * @param length Its length.
 * @param options NULL for the defaults.
 * @param diagnostic NULL, or receives what happened (OK in its `outcome` if the attribute was added).
 * @param result NULL, or filled as for gtext_css_candidates_add().
 * @return GTEXT_CSS_OK (added or dropped); GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_CANDIDATES; and the other statuses of
 *   gtext_css_candidates_add().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_candidates_add_presentation(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys,
    const GTEXT_CSS_Presentation_Row * row, const char * value, size_t length, const GTEXT_CSS_Presentation_Options * options,
    GTEXT_CSS_Presentation_Diagnostic * diagnostic, GTEXT_CSS_Cascade_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_PRESENTATION_H
