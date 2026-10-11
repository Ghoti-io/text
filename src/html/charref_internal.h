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
 * The HTML named character reference table, and the two lookups the tokenizer
 * makes over it.
 *
 * WHATWG HTML, "Named character references". The table itself is
 * tables/charref_table.c, generated from the pinned entities.json by
 * tools/html/gen_charrefs.py; this header is the shape that file names, so a
 * regeneration against a different layout fails to compile rather than
 * silently meaning something else.
 *
 * ## What a name is
 *
 * The text after the `&`. `AMP;` and `AMP` are two entries and are not folded
 * into one: 106 names exist both with and without the semicolon (HTML 4
 * browsers accepted a reference without it), and the tokenizer has to know
 * which spelling it matched, because the two behave differently in an
 * attribute value and are reported differently as errors.
 *
 * ## The one rule that makes lookup short
 *
 * Every name is one or more ASCII letters and digits, optionally followed by a
 * single `;`. tests/test-html-charref.cpp checks that for the whole table and
 * the generator's own tests check it for any new data, because
 * gtext_html_charref_longest_match() relies on it: the longest name that
 * can match at a position is bounded by the alphanumeric run there plus one,
 * so the search tries a handful of lengths and not all thirty-two.
 *
 * Internal. Nothing here is public API until the HTML tokenizer is.
 */

#ifndef GHOTI_IO_GTEXT_SRC_HTML_CHARREF_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_HTML_CHARREF_INTERNAL_H

#include <ghoti.io/text/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One named character reference.
 *
 * Fixed-size and pointer-free, so the shared library carries no relocations
 * for the 2,231 of them. The name is read through
 * gtext_html_charref_name(), not stored here.
 */
typedef struct {
  uint16_t name_offset; ///< Where the name starts in gtext_html_charref_names.
  uint8_t name_length;  ///< Bytes of the name, a trailing `;` included.
  uint8_t code_points;  ///< How many code points it expands to: 1 or 2.
  uint32_t first;       ///< The first code point.
  uint32_t second;      ///< The second, or 0 when code_points is 1.
} GTEXT_HTML_CharRef;

/// The entries, sorted ascending by the bytes of the name, no duplicates.
extern const GTEXT_HTML_CharRef gtext_html_charref_table[];
/// Every name run together, with no separators and no terminator.
extern const char gtext_html_charref_names[];
/// How many entries gtext_html_charref_table holds.
extern const size_t gtext_html_charref_count;
/// The length of the shortest name, a trailing `;` included.
extern const size_t gtext_html_charref_shortest;
/// The length of the longest name, a trailing `;` included.
extern const size_t gtext_html_charref_longest;

/**
 * @brief The bytes of an entry's name.
 *
 * Not NUL-terminated; its length is `entry->name_length`.
 *
 * @param entry An entry of gtext_html_charref_table. Not NULL.
 * @return The first byte of the name.
 */
static inline const char * gtext_html_charref_name(
    const GTEXT_HTML_CharRef * entry) {
  return gtext_html_charref_names + entry->name_offset;
}

/**
 * @brief Find the entry whose name is exactly these bytes.
 *
 * @param name The candidate name, without the `&`. Need not be terminated.
 * @param len Its length.
 * @return The entry, or NULL if there is none.
 */
GTEXT_INTERNAL_API const GTEXT_HTML_CharRef * gtext_html_charref_find(
    const char * name, size_t len);

/**
 * @brief Find the longest name that is a prefix of these bytes.
 *
 * This is the tokenizer's rule - "consume the maximum number of characters
 * possible, where the consumed characters are one of the identifiers in the
 * first column of the named character references table" - and it is a rule
 * about what is *consumed*, not about what is a good match. `&notit;` consumes
 * `not` and leaves `it;` for the text, because `notit;` is not a name and
 * `not` (no semicolon) is.
 *
 * @param text The bytes after an `&`. Need not be terminated.
 * @param len How many are readable. Never read past.
 * @return The entry for the longest matching name, or NULL if no name is a
 *   prefix of `text`.
 */
GTEXT_INTERNAL_API const GTEXT_HTML_CharRef *
gtext_html_charref_longest_match(const char * text, size_t len);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_HTML_CHARREF_INTERNAL_H
