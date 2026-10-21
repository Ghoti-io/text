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
 * @file css_serialize.h
 * @brief Writing a CSS tree, or any part of it, back out as text.
 *
 * CSS Syntax section 9 asks one thing of a serializer: that it round-trips,
 * "parsing the stylesheet must produce the same data structures as parsing,
 * serializing, and parsing again, except for consecutive whitespace-tokens,
 * which may be collapsed into a single token". This one does, and spells
 * what it writes as CSSOM's serializing idioms do:
 *
 * - an identifier (an ident's value, a function's name, an at-keyword, a
 *   dimension's unit, an id hash) by CSSOM's "serialize an identifier";
 * - a string by "serialize a string" (always in double quotes);
 * - two tokens that would read back as one, by the table of CSS Syntax 9,
 *   with an empty comment between them;
 * - a `<delim-token>` of U+005C by U+005C and a newline.
 *
 * Where CSSOM's idiom would not read back as the same token it is not used,
 * and the manual (documentation/formats/css.md) lists each case: an unquoted
 * url is written as one (CSSOM's "serialize a URL" writes a string, which
 * reads back as a function), a function keeps the case of its name, an
 * unrestricted hash does not escape a leading digit, and the few token
 * sequences the table of section 9 does not cover are separated as well.
 *
 * Whitespace tokens are written as one space; nothing else adds white space,
 * except one space before `!important`.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_SERIALIZE_H
#define GHOTI_IO_GTEXT_CSS_CSS_SERIALIZE_H

#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How numeric tokens are written. */
typedef enum {
  /** As they were written: the token's `repr`, which keeps the type flag
   *  and the sign. The default, and the mode the round trip is for. */
  GTEXT_CSS_NUMBERS_SOURCE = 0,
  /** As CSSOM serializes a `<number>` (and an `<integer>`): base ten, no
   *  exponent, at most six decimals, a `-` only for a negative value. The
   *  type flag and a `+` sign are not kept, so the text may read back as a
   *  different token (`1.0` is written `1`, which is an integer). */
  GTEXT_CSS_NUMBERS_CSSOM
} GTEXT_CSS_Number_Style;

/**
 * @struct GTEXT_CSS_Serialize_Options
 * @brief How to write. A zeroed struct is the default.
 */
typedef struct {
  /** The allocator of the buffer forms, or NULL for the default. */
  const GTEXT_Allocator * allocator;
  /** How numbers are spelled. Default GTEXT_CSS_NUMBERS_SOURCE. */
  GTEXT_CSS_Number_Style numbers;
  /** Most bytes of output; one more fails with GTEXT_CSS_E_LIMIT_OUTPUT
   *  (the sink has had the output up to the limit). 0 is the default,
   *  256 MiB. */
  size_t max_output_bytes;
} GTEXT_CSS_Serialize_Options;

/** @brief The defaults: no allocator, source numbers, 256 MiB. */
GTEXT_API GTEXT_CSS_Serialize_Options gtext_css_serialize_options_default(void);

/**
 * @brief Write a whole tree: what its entry point returned, in a form that
 *   entry point reads back.
 *
 * A stylesheet's rules, a list of declarations (each followed by `;`), a
 * block's contents, one rule, one declaration, one value, a list of values,
 * or the groups of a comma-separated list (joined by `,`). A tree that holds
 * a syntax error writes nothing.
 *
 * @param tree The tree. Not NULL.
 * @param options NULL for the defaults.
 * @param sink Where the text goes. Not NULL, with a `write`.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_WRITE when the sink
 *   refused (the sink's own status is not kept); GTEXT_CSS_E_LIMIT_OUTPUT.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_tree(const GTEXT_CSS_Tree * tree,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink);

/** @brief Write one rule. `nested` says whether it is read back inside a
 *  block (it matters for a qualified rule whose prelude could read as a
 *  declaration's name and colon). */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_rule(const GTEXT_CSS_Rule * rule,
    bool nested, const GTEXT_CSS_Serialize_Options * options,
    const GTEXT_Markup_Sink * sink);

/** @brief Write one declaration, without a trailing `;`. */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_declaration(
    const GTEXT_CSS_Declaration * declaration,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink);

/** @brief Write a list of component values (NULL with a count of 0 is
 *  empty), as parse a list of component values reads them back. */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_values(
    const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink);

/**
 * @brief Write a tree to a new buffer.
 *
 * @param tree The tree. Not NULL.
 * @param options NULL for the defaults; its allocator makes the buffer.
 * @param out Receives the text, NUL-terminated (not counted in `out_length`);
 *   release it with gtext_css_serialize_buffer_free(). Written only on success.
 * @param out_length Receives its length. Not NULL.
 * @return As gtext_css_serialize_tree(), or GTEXT_CSS_E_OOM; a failure leaves
 *   no buffer.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_tree_buffer(
    const GTEXT_CSS_Tree * tree, const GTEXT_CSS_Serialize_Options * options,
    char ** out, size_t * out_length);

/** @brief Write component values to a new buffer (as the tree form). */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_values_buffer(
    const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Serialize_Options * options, char ** out,
    size_t * out_length);

/**
 * @brief Release a buffer from a `_buffer` function.
 *
 * @param options The options it was written with (their allocator frees it);
 *   NULL for the default allocator.
 * @param buffer The buffer; NULL is ignored.
 */
GTEXT_API void gtext_css_serialize_buffer_free(
    const GTEXT_CSS_Serialize_Options * options, char * buffer);

/**
 * @brief CSSOM's "serialize an identifier".
 *
 * @param text UTF-8 (ill-formed sequences are written as U+FFFD).
 * @param length Its length; 0 writes nothing.
 * @param sink Where it goes.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_WRITE.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_identifier(const char * text,
    size_t length, const GTEXT_Markup_Sink * sink);

/** @brief CSSOM's "serialize a string": `"`, the text escaped, `"`. */
GTEXT_API GTEXT_CSS_Status gtext_css_serialize_string(const char * text,
    size_t length, const GTEXT_Markup_Sink * sink);

/**
 * @brief CSS Syntax 9.1, "serialize an `<an-plus-b>` value".
 *
 * @param a A.
 * @param b B.
 * @param buffer Receives the text, NUL-terminated (at most 43 bytes with the NUL).
 * @param size Its size; 44 is always enough.
 * @return The length written, or 0 if `buffer` is NULL or too small.
 */
GTEXT_API size_t gtext_css_anb_serialize(int64_t a, int64_t b, char * buffer,
    size_t size);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_SERIALIZE_H
