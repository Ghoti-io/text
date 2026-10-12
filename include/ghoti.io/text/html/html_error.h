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
 * @file html_error.h
 * @brief The parse errors of an HTML parse, and how to read them.
 *
 * A parse error is the standard's term for a mistake in the document: the
 * parser reports it and goes on, with a defined repair, so **a parse error is
 * never a failure** and every input has a tree. The parse errors of a parse
 * are in its @ref GTEXT_HTML_Result, in the order they were raised, up to
 * `max_errors`; the rest are only counted.
 *
 * ## Codes and names
 *
 * Each kind of error has a code, a stable number, and a name that is
 * spelled as the standard spells it (`duplicate-attribute`,
 * `end-tag-with-attributes`). No two codes share a name, and the numbers do not
 * change between releases. Codes 0 to 52 are the tokenizer's list (one of them,
 * number 41, is raised by the tree builder) and 64 to 79 are the tree builder's, so
 * that a new code of either kind has room.
 *
 * - 0 to 51 are the standard's own list ("Parse errors", WHATWG HTML 13.2.2),
 *   in the order below. The tokenizer raises 51 of them; the 52nd,
 *   `non-void-html-element-start-tag-with-trailing-solidus` (41), is raised
 *   by the tree builder.
 * - 52 is `invalid-utf-8-in-input-stream`, which is not the standard's: it
 *   names the replacement of an invalid byte sequence by U+FFFD, which the
 *   standard's decoder step leaves without a name.
 * - 64 to 79 are the tree builder's. The standard says "this is a parse
 *   error" in these places without naming them; these are named for what
 *   happened. One of them is the standard's name for a tokenizer error with
 *   a suffix: a U+0000 where the tree builder does not want one is
 *   `unexpected-null-character-in-content`, distinct from the tokenizer's
 *   `unexpected-null-character`.
 *
 * ## Positions
 *
 * `line` and `column` are 1-based. A line ends at LF, CR or CR LF. The
 * column counts **UTF-16 code units** (a character outside the Basic
 * Multilingual Plane is 2), as the positions the standard's test suites use
 * and as a DOM script reads a position; `byte_offset` is 0-based into the
 * bytes the parser was given, a byte order mark included. Where the
 * position is the construct's, it is its first character; where a rule
 * applies to the token being handled, it is that token's. The position of
 * each tokenizer error is the one the html5lib tests give.
 */

#ifndef GHOTI_IO_GTEXT_HTML_HTML_ERROR_H
#define GHOTI_IO_GTEXT_HTML_HTML_ERROR_H

#include <ghoti.io/text/html/html_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_HTML_Error_Code
 * @brief Every parse error, by number.
 *
 * The comment on each is the name gtext_html_error_name() gives. Numbers 0 to 52
 * are the tokenizer's list (the tree builder raises number 41); they are the standard's, but 52: see "Parse errors" in the
 * WHATWG HTML Living Standard for each. The tree builder's (64 to 79) are
 * described here.
 */
typedef enum {
  GTEXT_HTML_ERROR_ABRUPT_CLOSING_OF_EMPTY_COMMENT = 0, ///< `abrupt-closing-of-empty-comment` (the tokenizer)
  GTEXT_HTML_ERROR_ABRUPT_DOCTYPE_PUBLIC_IDENTIFIER = 1, ///< `abrupt-doctype-public-identifier` (the tokenizer)
  GTEXT_HTML_ERROR_ABRUPT_DOCTYPE_SYSTEM_IDENTIFIER = 2, ///< `abrupt-doctype-system-identifier` (the tokenizer)
  GTEXT_HTML_ERROR_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE = 3, ///< `absence-of-digits-in-numeric-character-reference` (the tokenizer)
  GTEXT_HTML_ERROR_CDATA_IN_HTML_CONTENT = 4, ///< `cdata-in-html-content` (the tokenizer)
  GTEXT_HTML_ERROR_CHARACTER_REFERENCE_OUTSIDE_UNICODE_RANGE = 5, ///< `character-reference-outside-unicode-range` (the tokenizer)
  GTEXT_HTML_ERROR_CONTROL_CHARACTER_IN_INPUT_STREAM = 6, ///< `control-character-in-input-stream` (the tokenizer)
  GTEXT_HTML_ERROR_CONTROL_CHARACTER_REFERENCE = 7, ///< `control-character-reference` (the tokenizer)
  GTEXT_HTML_ERROR_DISALLOWED_PROCESSING_INSTRUCTION_TARGET = 8, ///< `disallowed-processing-instruction-target` (the tokenizer)
  GTEXT_HTML_ERROR_DUPLICATE_ATTRIBUTE = 9, ///< `duplicate-attribute` (the tokenizer)
  GTEXT_HTML_ERROR_END_TAG_WITH_ATTRIBUTES = 10, ///< `end-tag-with-attributes` (the tokenizer)
  GTEXT_HTML_ERROR_END_TAG_WITH_TRAILING_SOLIDUS = 11, ///< `end-tag-with-trailing-solidus` (the tokenizer)
  GTEXT_HTML_ERROR_EOF_BEFORE_TAG_NAME = 12, ///< `eof-before-tag-name` (the tokenizer)
  GTEXT_HTML_ERROR_EOF_IN_CDATA = 13, ///< `eof-in-cdata` (the tokenizer)
  GTEXT_HTML_ERROR_EOF_IN_COMMENT = 14, ///< `eof-in-comment` (the tokenizer)
  GTEXT_HTML_ERROR_EOF_IN_DOCTYPE = 15, ///< `eof-in-doctype` (the tokenizer)
  GTEXT_HTML_ERROR_EOF_IN_PROCESSING_INSTRUCTION = 16, ///< `eof-in-processing-instruction` (the tokenizer)
  GTEXT_HTML_ERROR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT = 17, ///< `eof-in-script-html-comment-like-text` (the tokenizer)
  GTEXT_HTML_ERROR_EOF_IN_TAG = 18, ///< `eof-in-tag` (the tokenizer)
  GTEXT_HTML_ERROR_INCORRECTLY_CLOSED_COMMENT = 19, ///< `incorrectly-closed-comment` (the tokenizer)
  GTEXT_HTML_ERROR_INCORRECTLY_OPENED_COMMENT = 20, ///< `incorrectly-opened-comment` (the tokenizer)
  GTEXT_HTML_ERROR_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME = 21, ///< `invalid-character-sequence-after-doctype-name` (the tokenizer)
  GTEXT_HTML_ERROR_INVALID_FIRST_CHARACTER_OF_PROCESSING_INSTRUCTION_TARGET = 22, ///< `invalid-first-character-of-processing-instruction-target` (the tokenizer)
  GTEXT_HTML_ERROR_INVALID_FIRST_CHARACTER_OF_TAG_NAME = 23, ///< `invalid-first-character-of-tag-name` (the tokenizer)
  GTEXT_HTML_ERROR_INVALID_PROCESSING_INSTRUCTION_TARGET = 24, ///< `invalid-processing-instruction-target` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_ATTRIBUTE_VALUE = 25, ///< `missing-attribute-value` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_DOCTYPE_NAME = 26, ///< `missing-doctype-name` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_DOCTYPE_PUBLIC_IDENTIFIER = 27, ///< `missing-doctype-public-identifier` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_DOCTYPE_SYSTEM_IDENTIFIER = 28, ///< `missing-doctype-system-identifier` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_END_TAG_NAME = 29, ///< `missing-end-tag-name` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER = 30, ///< `missing-quote-before-doctype-public-identifier` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER = 31, ///< `missing-quote-before-doctype-system-identifier` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE = 32, ///< `missing-semicolon-after-character-reference` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_WHITESPACE_AFTER_DOCTYPE_PUBLIC_KEYWORD = 33, ///< `missing-whitespace-after-doctype-public-keyword` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_WHITESPACE_AFTER_DOCTYPE_SYSTEM_KEYWORD = 34, ///< `missing-whitespace-after-doctype-system-keyword` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_WHITESPACE_BEFORE_DOCTYPE_NAME = 35, ///< `missing-whitespace-before-doctype-name` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_WHITESPACE_BETWEEN_ATTRIBUTES = 36, ///< `missing-whitespace-between-attributes` (the tokenizer)
  GTEXT_HTML_ERROR_MISSING_WHITESPACE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS = 37, ///< `missing-whitespace-between-doctype-public-and-system-identifiers` (the tokenizer)
  GTEXT_HTML_ERROR_NESTED_COMMENT = 38, ///< `nested-comment` (the tokenizer)
  GTEXT_HTML_ERROR_NONCHARACTER_CHARACTER_REFERENCE = 39, ///< `noncharacter-character-reference` (the tokenizer)
  GTEXT_HTML_ERROR_NONCHARACTER_IN_INPUT_STREAM = 40, ///< `noncharacter-in-input-stream` (the tokenizer)
  GTEXT_HTML_ERROR_NON_VOID_HTML_ELEMENT_START_TAG_WITH_TRAILING_SOLIDUS = 41, ///< `non-void-html-element-start-tag-with-trailing-solidus` (the tree builder raises it)
  GTEXT_HTML_ERROR_NULL_CHARACTER_REFERENCE = 42, ///< `null-character-reference` (the tokenizer)
  GTEXT_HTML_ERROR_SURROGATE_CHARACTER_REFERENCE = 43, ///< `surrogate-character-reference` (the tokenizer)
  GTEXT_HTML_ERROR_SURROGATE_IN_INPUT_STREAM = 44, ///< `surrogate-in-input-stream` (the tokenizer)
  GTEXT_HTML_ERROR_UNEXPECTED_CHARACTER_AFTER_DOCTYPE_SYSTEM_IDENTIFIER = 45, ///< `unexpected-character-after-doctype-system-identifier` (the tokenizer)
  GTEXT_HTML_ERROR_UNEXPECTED_CHARACTER_IN_ATTRIBUTE_NAME = 46, ///< `unexpected-character-in-attribute-name` (the tokenizer)
  GTEXT_HTML_ERROR_UNEXPECTED_CHARACTER_IN_UNQUOTED_ATTRIBUTE_VALUE = 47, ///< `unexpected-character-in-unquoted-attribute-value` (the tokenizer)
  GTEXT_HTML_ERROR_UNEXPECTED_EQUALS_SIGN_BEFORE_ATTRIBUTE_NAME = 48, ///< `unexpected-equals-sign-before-attribute-name` (the tokenizer)
  GTEXT_HTML_ERROR_UNEXPECTED_NULL_CHARACTER = 49, ///< `unexpected-null-character` (the tokenizer)
  GTEXT_HTML_ERROR_UNEXPECTED_SOLIDUS_IN_TAG = 50, ///< `unexpected-solidus-in-tag` (the tokenizer)
  GTEXT_HTML_ERROR_UNKNOWN_NAMED_CHARACTER_REFERENCE = 51, ///< `unknown-named-character-reference` (the tokenizer)
  GTEXT_HTML_ERROR_INVALID_UTF_8_IN_INPUT_STREAM = 52, ///< `invalid-utf-8-in-input-stream` (the tokenizer)

  GTEXT_HTML_ERROR_MISSING_DOCTYPE = 64, ///< `missing-doctype`: No DOCTYPE before the first token that is not a comment.
  GTEXT_HTML_ERROR_NON_CONFORMING_DOCTYPE = 65, ///< `non-conforming-doctype`: A DOCTYPE that is not `<!DOCTYPE html>` (or its legacy forms).
  GTEXT_HTML_ERROR_UNEXPECTED_DOCTYPE = 66, ///< `unexpected-doctype`: A DOCTYPE anywhere but the initial mode.
  GTEXT_HTML_ERROR_UNEXPECTED_START_TAG = 67, ///< `unexpected-start-tag`: A start tag the mode ignores or repairs.
  GTEXT_HTML_ERROR_UNEXPECTED_END_TAG = 68, ///< `unexpected-end-tag`: An end tag with no matching element in scope, or the mode ignores.
  GTEXT_HTML_ERROR_UNEXPECTED_CHARACTER = 69, ///< `unexpected-character`: Text where the mode has none.
  GTEXT_HTML_ERROR_UNEXPECTED_NULL_CHARACTER_IN_CONTENT = 70, ///< `unexpected-null-character-in-content`: A U+0000 in body content.
  GTEXT_HTML_ERROR_UNEXPECTED_EOF = 71, ///< `unexpected-eof`: The input ended in the "text" mode or the head's noscript.
  GTEXT_HTML_ERROR_OPEN_ELEMENTS_AT_EOF = 72, ///< `open-elements-at-eof`: The input ended with elements open that need an end tag.
  GTEXT_HTML_ERROR_OPEN_ELEMENTS_AT_END_TAG = 73, ///< `open-elements-at-end-tag`: An end tag closed an element that had open children.
  GTEXT_HTML_ERROR_MISNESTED_FORMATTING = 74, ///< `misnested-formatting`: `a` in `a`, `nobr` in `nobr`, or the adoption agency's repairs.
  GTEXT_HTML_ERROR_NESTED_FORM = 75, ///< `nested-form`: A `form` start tag inside a form.
  GTEXT_HTML_ERROR_UNEXPECTED_ELEMENT_IN_CONTEXT = 76, ///< `unexpected-element-in-context`: `h1` in `h1`, a `button` in a button, `select` in `select`...
  GTEXT_HTML_ERROR_DEPRECATED_TAG = 77, ///< `deprecated-tag`: `image` read as `img`; `</br>` read as `<br>`.
  GTEXT_HTML_ERROR_HTML_IN_FOREIGN_CONTENT = 78, ///< `html-in-foreign-content`: An HTML start tag (or `</p>`, `</br>`) that ends foreign content.
  GTEXT_HTML_ERROR_XMLNS_ATTRIBUTE_MISMATCH = 79, ///< `xmlns-attribute-mismatch`: An `xmlns` attribute that is not the element's namespace.
} GTEXT_HTML_Error_Code;

/**
 * @struct GTEXT_HTML_Error
 * @brief One parse error, with its position.
 */
typedef struct {
  GTEXT_HTML_Error_Code code; ///< Which kind.
  const char * name;          ///< Its name, as gtext_html_error_name() gives it.
                              ///< Static; never freed.
  size_t line;                ///< Line (1-based).
  size_t column;              ///< Column, in UTF-16 code units (1-based).
  size_t byte_offset;         ///< Byte offset into the input (0-based).
} GTEXT_HTML_Error;

/**
 * @brief The name of an error code.
 *
 * @param code Any value; one that is not a code returns "unknown".
 * @return A static string, never freed. Different for every code.
 */
GTEXT_API const char * gtext_html_error_name(GTEXT_HTML_Error_Code code);

/**
 * @brief How many parse errors the result holds.
 *
 * At most `max_errors`, and 0 for `count_errors_only`; see
 * gtext_html_result_errors_dropped() for the rest.
 *
 * @param result A result a parse filled in. NULL is 0.
 */
GTEXT_API size_t gtext_html_result_error_count(const GTEXT_HTML_Result * result);

/**
 * @brief How many parse errors were raised and not kept.
 *
 * The kept and the dropped together are every parse error of the parse (or, for
 * a failed parse, every one raised before it failed), however `max_errors`
 * was set.
 *
 * @param result A result a parse filled in. NULL is 0.
 */
GTEXT_API size_t gtext_html_result_errors_dropped(const GTEXT_HTML_Result * result);

/**
 * @brief One kept parse error, in the order raised.
 *
 * @param result A result a parse filled in.
 * @param index  0 up to (not including) gtext_html_result_error_count().
 * @param out    Receives the error. Written only on success.
 * @return true, or false for an index out of range or a NULL argument.
 */
GTEXT_API bool gtext_html_result_error_at(const GTEXT_HTML_Result * result,
    size_t index, GTEXT_HTML_Error * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_HTML_HTML_ERROR_H
