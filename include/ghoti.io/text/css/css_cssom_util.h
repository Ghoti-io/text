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
 * @brief The `CSS` namespace functions that need no document: `CSS.escape()` (CSS Object Model, "CSS.escape()").
 *
 * `CSS.supports()` and `matchMedia()` are bound to a document (css_cssom_document.h).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_UTIL_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_UTIL_H

#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the text of gtext_cssom_escape() is read as. */
typedef enum {
  GTEXT_CSSOM_TEXT_UTF8 = 0,   ///< Well-formed UTF-8; an ill-formed sequence is GTEXT_CSS_E_INVALID (the text API takes well-formed text).
  GTEXT_CSSOM_TEXT_WTF8        ///< WTF-8: a lone surrogate (`ED A0 80` to `ED BF BF`) is one code point, written as the escape of its code point, as a browser holding a JavaScript string with a lone surrogate does.
} GTEXT_CSSOM_Text_Encoding;

/**
 * @brief `CSS.escape(text)`: CSSOM's "serialize an identifier" of the text.
 *
 * The empty text writes nothing. A NUL (U+0000) is U+FFFD. A control character (U+0001 to U+001F, U+007F) is a hex escape; a leading digit, and a digit after a
 * leading hyphen, is a hex escape; a lone hyphen is `\-`; a character that is not an identifier character is escaped with a backslash; every other code point is
 * written as it is. The result is always an identifier that reads back as the text.
 *
 * @param text UTF-8 (or WTF-8, see `encoding`). NULL only with `length` 0.
 * @param length Its length in bytes.
 * @param encoding How the text is read.
 * @param sink Where the result goes. Not NULL, with a `write`.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, or ill-formed text: nothing is written); GTEXT_CSS_E_WRITE; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_escape(const char * text, size_t length, GTEXT_CSSOM_Text_Encoding encoding, const GTEXT_Markup_Sink * sink);

/**
 * @brief gtext_cssom_escape() to a new NUL-terminated buffer.
 * @param allocator The allocator; NULL for the default. Release the buffer with gtext_cssom_buffer_free() given the same allocator (css_cssom_core.h).
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_escape_buffer(const char * text, size_t length, GTEXT_CSSOM_Text_Encoding encoding, const GTEXT_Allocator * allocator,
    char ** out, size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_UTIL_H
