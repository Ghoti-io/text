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
 * @file css_decode.h
 * @brief Stylesheets as bytes: the standard's decode, with offsets in the
 *   bytes.
 *
 * A stylesheet arrives as bytes (a file, a response, the content of a
 * `<style>` element) and CSS Syntax 3.2 says how they become the code points
 * the tokenizer reads: "determine the fallback encoding" and then the Encoding
 * Standard's "decode", which gives a byte order mark precedence over
 * everything. gtext_css_parse_stylesheet_bytes() does both and parses, and
 * every position it reports is in the **caller's bytes**, whatever the
 * encoding: the spans of the tree, the byte offsets of the parse errors and the
 * place of a limit. (Lines and columns count the decoded text's characters, as
 * the HTML module's do for a document in another encoding.)
 *
 * ## The encoding
 *
 * In order, the first that applies:
 *
 * 1. A **byte order mark** (EF BB BF, FE FF or FF FE): UTF-8, UTF-16BE or
 *    UTF-16LE, over anything below.
 * 2. The **protocol's label** (an HTTP `charset`), if it names an encoding.
 *    A label that names none (`kamoulox`) is ignored, not an error.
 * 3. A leading **`@charset "...";`** in the first 1024 bytes, byte for byte:
 *    `@charset "`, any bytes from 0x01 to 0x7F except `"`, then `";` (so no
 *    second space, no single quotes, no comment, no `@Charset`), and a label
 *    that names an encoding. UTF-16BE and UTF-16LE become UTF-8, because
 *    those bytes spelled the declaration in ASCII, which UTF-16 is not. A
 *    declaration whose label names nothing is ignored.
 * 4. The **environment encoding** the caller passes, if any (HTML gives one
 *    for a `<link rel=stylesheet>` from its own document's encoding).
 * 5. UTF-8.
 *
 * The result says which (GTEXT_CSS_Result::encoding and `encoding_source`).
 * The `@charset` is not a rule: it is read from the bytes and the text is
 * parsed as it stands, so the tree has the generic at-rule named `charset`
 * that the syntax gives it, which a layer that checks rules against grammars
 * drops (CSS Syntax 8.3).
 *
 * ## What is read in place
 *
 * UTF-8 (a byte order mark skipped) is parsed in the caller's bytes without a
 * copy, ill-formed sequences reported as `invalid-utf-8` parse errors. Every
 * other encoding is decoded into a copy first, and a byte the encoding cannot
 * map is the replacement character, with no parse error of its own. That text
 * can be three times the size of the bytes, and `max_input_bytes` applies to
 * the bytes, not to the text. A validity callback sees positions in the
 * decoded text (they are the bytes' own only for UTF-8 with no byte order
 * mark), not in the bytes; the tree and the result the call returns are in the
 * bytes.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_DECODE_H
#define GHOTI_IO_GTEXT_CSS_CSS_DECODE_H

#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/encoding/encoding_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct GTEXT_CSS_Bytes_Options
 * @brief What the caller knows about a stylesheet's bytes besides the bytes.
 *
 * A zeroed struct knows nothing: no label, no environment encoding.
 * Fields are only ever appended, with a zero default.
 */
typedef struct {
  /** The encoding label the protocol gave (an HTTP `Content-Type` `charset`
   *  parameter, without its quotes), NUL-terminated, or NULL for none. Leading
   *  and trailing ASCII white space is ignored and the match does not depend on
   *  case, as the Encoding Standard's "get an encoding" has it. */
  const char * protocol_label;

  /** The environment encoding the referring document provides, or
   *  GTEXT_ENCODING_NONE (0) for none. */
  GTEXT_Encoding environment_encoding;
} GTEXT_CSS_Bytes_Options;

/**
 * @brief The defaults: no label, no environment encoding.
 *
 * @return By value.
 */
GTEXT_API GTEXT_CSS_Bytes_Options gtext_css_bytes_options_default(void);

/**
 * @struct GTEXT_CSS_Encoding_Decision
 * @brief What gtext_css_determine_encoding() found.
 */
typedef struct {
  GTEXT_Encoding encoding;           ///< The encoding the bytes are decoded as.
  GTEXT_CSS_Encoding_Source source;  ///< Why.
  size_t bom_length;                 ///< Bytes of byte order mark at the start (0, 2 or 3).
} GTEXT_CSS_Encoding_Decision;

/**
 * @brief The number of bytes at the start of a stylesheet that decide its
 *   encoding (CSS Syntax 3.2): 1024.
 */
#define GTEXT_CSS_ENCODING_WINDOW 1024

/**
 * @brief Decide how a stylesheet's bytes are decoded (the section's
 *   algorithm, with a byte order mark first).
 *
 * Only the first GTEXT_CSS_ENCODING_WINDOW bytes are looked at, so the answer
 * is the same for the whole stylesheet and for its first kilobyte.
 *
 * @param bytes  The stylesheet; NULL only with `length` 0.
 * @param length Its length (a longer one than the window is fine).
 * @param options NULL for none.
 * @param out    Receives the decision; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, or GTEXT_CSS_E_INVALID for a NULL output or NULL
 *   bytes with a length.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_determine_encoding(const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * options, GTEXT_CSS_Encoding_Decision * out);

/**
 * @brief Parse a stylesheet from bytes (CSS Syntax 3.2 and 5.4.3).
 *
 * The bytes are decoded as above and parsed as gtext_css_parse_stylesheet()
 * parses text. The tree, the parse errors and the verdict are the same as
 * those of a gtext_css_stream_new_bytes() stream given the same bytes in
 * pieces of any sizes.
 *
 * `max_input_bytes` limits the bytes. A stylesheet longer than it fails with
 * GTEXT_CSS_E_LIMIT_INPUT, the result's `failure` at the byte where the limit
 * is reached; its line and column are 0, because nothing was decoded.
 *
 * @param bytes  The stylesheet; NULL only with `length` 0.
 * @param length Its length in bytes.
 * @param bytes_options NULL for none.
 * @param options NULL for the defaults.
 * @param out    Receives the tree, written only on success. Not NULL.
 * @param result NULL, or filled whatever happens (free it with
 *               gtext_css_result_free()); its `encoding`, `encoding_source`
 *               and `bom_length` say how the bytes were read.
 * @return As gtext_css_parse_stylesheet().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_stylesheet_bytes(const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_DECODE_H
