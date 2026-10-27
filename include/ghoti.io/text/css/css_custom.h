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
 * @file css_custom.h
 * @brief The computed value of a custom property: an immutable token stream, with the guaranteed-invalid value and the empty value kept apart.
 *
 * Reference: CSS Custom Properties for Cascading Variables Level 1 (served 2026-10-26), sections 2 and 4.1; CSS Properties and Values API Level 1
 * (6 August 2026), 2.4 and 2.7; CSS Values and Units Level 5, Appendix A.
 *
 * ## What a custom property's value is
 *
 * A custom property (`--name`) has no grammar: its value is the stream of tokens that was written, "exactly as specified by the author" (Variables 1, 4.1), so
 * `--uuid: 12345678-12e3-8d9b-a456-426614174000` comes back as it went in, comments and white space between its tokens included. Its **computed value** is
 * that stream with every `var()` and the other arbitrary substitution functions replaced by what they stand for, or the **guaranteed-invalid value**
 * ("the initial value of a custom property"; also what a cycle, a missing variable without a fallback and a value that would be too long make). The
 * guaranteed-invalid value "serializes as the empty string, but actually writing an empty value into a custom property, like `--foo:;`, is a valid
 * (empty) value, not the guaranteed-invalid value": here the first is **no value** (the accessors say so by returning false or NULL) and the second is a
 * @ref GTEXT_CSS_Custom_Value of no bytes.
 *
 * A registered custom property (css_registry.h, css_at_property.h) with a syntax other than `*` has as its computed value the tokens its computed typed value
 * is written as (`--x: 8em` with `<length>` at a `font-size` of 10px is `80px`).
 *
 * ## Why it is not a string
 *
 * Substituting a `var()` copies a value, and copying doubles it: `--b: var(--a) var(--a); --c: var(--b) var(--b)` and so on is the "billion laughs" of CSS. A value
 * here is a **rope** (private): an immutable tree whose two halves of a concatenation may be the same node, so that memory stays linear in the text that was
 * written, and the length the value would be written as (gtext_css_custom_value_length(), gtext_css_custom_value_tokens()) is a number that saturates. The text
 * is only made when something asks for it, and the library refuses to make a value whose expanded length passes the budgets of css_substitute.h before anyone could.
 *
 * ## Ownership
 *
 * A value is reference counted (atomically) and immutable: a computed style holds one for each custom property, the ones it inherits are the parent's own,
 * and a value outlives the style that made it for as long as anyone holds it. Any number of threads may read one.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CUSTOM_H
#define GHOTI_IO_GTEXT_CSS_CSS_CUSTOM_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The computed value of a custom property (the struct is private). Absent (NULL) is the guaranteed-invalid value. */
typedef struct GTEXT_CSS_Custom_Value GTEXT_CSS_Custom_Value;

/**
 * @brief A value of these bytes (copied). The text is taken as the tokens of the value: nothing is parsed or checked, and the empty text is the empty value.
 *
 * @param allocator NULL for the default.
 * @param text The bytes; need not be terminated. NULL only with a length of 0.
 * @param length Their number.
 * @param out Receives the value, with one owner; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_custom_value_new(const GTEXT_Allocator * allocator, const char * text, size_t length, GTEXT_CSS_Custom_Value ** out);

/** @brief Another owner of a value (NULL for NULL). */
GTEXT_API GTEXT_CSS_Custom_Value * gtext_css_custom_value_retain(const GTEXT_CSS_Custom_Value * value);

/** @brief One owner fewer; the last frees the value and what only it holds. NULL is ignored. */
GTEXT_API void gtext_css_custom_value_free(GTEXT_CSS_Custom_Value * value);

/** @brief The number of bytes the value is written as; SIZE_MAX if that is more than a size can say (a value that is held but could not be written). NULL is 0. */
GTEXT_API size_t gtext_css_custom_value_length(const GTEXT_CSS_Custom_Value * value);

/** @brief The number of tokens (white space is one) it is made of; saturates at SIZE_MAX. NULL is 0. */
GTEXT_API size_t gtext_css_custom_value_tokens(const GTEXT_CSS_Custom_Value * value);

/**
 * @brief Whether some of it came from an `attr()` (Values 5, 8.7.2: "attr()-tainted"). A tainted token stream is not allowed to become a URL.
 */
GTEXT_API bool gtext_css_custom_value_tainted(const GTEXT_CSS_Custom_Value * value);

/**
 * @brief Write the value to a sink, in pieces, in order.
 *
 * @param value The value. Not NULL.
 * @param sink Where the text goes. Not NULL, with a `write`.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM (the walk's stack); GTEXT_CSS_E_WRITE.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_custom_value_write(const GTEXT_CSS_Custom_Value * value, const GTEXT_Markup_Sink * sink);

/**
 * @brief Write the value to a new buffer, NUL-terminated (the NUL is not counted in `out_length`).
 *
 * @param value The value. Not NULL.
 * @param options NULL for the defaults; its allocator makes the buffer and its `max_output_bytes` bounds it.
 * @param out Receives the text; release it with gtext_css_serialize_buffer_free(). Written only on success.
 * @param out_length Receives its length. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_OUTPUT (the value is longer than `max_output_bytes`).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_custom_value_text(const GTEXT_CSS_Custom_Value * value, const GTEXT_CSS_Serialize_Options * options, char ** out,
    size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CUSTOM_H
