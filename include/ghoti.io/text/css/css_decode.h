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
 * place of a limit. (Lines and columns, those of the parse errors and of a
 * limit's `failure`, count the decoded text's characters, as the HTML module's
 * do for a document in another encoding; they are not moved into the bytes. A
 * failure that has no place, a failed allocation, has offset, line and column 0.)
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
 *    `@charset "`, any bytes from 0x00 to 0x7F except `"` (the draft's "a value
 *    between 0 and 21 inclusive or a value between 23 and 7F inclusive", in
 *    hexadecimal), then `";` (so no
 *    second space, no single quotes, no comment, no `@Charset`), and a label
 *    that names an encoding. UTF-16BE and UTF-16LE become UTF-8, because
 *    those bytes spelled the declaration in ASCII, which UTF-16 is not. A
 *    declaration whose label names nothing is ignored.
 * 4. The **environment encoding** the caller passes, if any (HTML gives one
 *    for a `<link rel=stylesheet>` from its own document's encoding). A value
 *    that is not an encoding (not GTEXT_ENCODING_NONE and not below
 *    GTEXT_ENCODING_COUNT) is an argument the caller got wrong:
 *    GTEXT_CSS_E_INVALID, with the result filled. Every encoding below
 *    GTEXT_ENCODING_COUNT has a decoder.
 * 5. UTF-8.
 *
 * The result says which (GTEXT_CSS_Result::encoding and `encoding_source`).
 * The `@charset` is not a rule: it is read from the bytes and the text is
 * parsed as it stands, so the tree has the generic at-rule named `charset`
 * that the syntax gives it, which a layer that checks rules against grammars
 * drops (CSS Syntax 8.3).
 *
 * ## What is read in place, and what the rest costs
 *
 * UTF-8 (a byte order mark skipped) is parsed in the caller's bytes without a
 * copy, ill-formed sequences reported as `invalid-utf-8` parse errors. Every
 * other encoding is decoded into a copy first, and a byte the encoding cannot
 * map is the replacement character, with no parse error of its own. The copy
 * is the decoded text and a map of where each code point came from, and both
 * are bounded by the options, so that `max_input_bytes`, which counts the
 * bytes, bounds them too:
 *
 * - the text is at most `max_decoded_bytes` (default **3 bytes for each byte of
 *   input**: a code point is at most 3 bytes of text for each byte of source
 *   it was read from; a lone replaced byte is the worst);
 * - the map is at most `max_map_runs` runs of 24 bytes (default **1 run for
 *   each byte of input**, reached only by text that alternates between a
 *   one-byte and a two-byte character, in every character; ASCII in any
 *   ASCII-compatible encoding is one run in all).
 *
 * The worst case is therefore about **27 bytes of memory for each byte of
 * input** (3 of text, 24 of map; the 8 bytes per 4096 of the lookup index are
 * nothing), and about 1 for ordinary text; UTF-8 read in place costs none. A
 * stylesheet that passes either bound fails with GTEXT_CSS_E_LIMIT_DECODED,
 * the result's `failure` at the byte of the code point that went past. Lower
 * either option to bound the memory more tightly than the input limit does.
 *
 * A validity callback sees positions in the **decoded text** (they are the
 * bytes' own only for UTF-8 with no byte order mark), not in the bytes, while
 * the tree and the result the call returns are in the bytes. The callbacks have
 * no way to map an offset back yet; the layers that use them (the grammar
 * engine) are owed one.
 *
 * ## The other entry points
 *
 * The one-call function reads a stylesheet. The other entry points of the syntax
 * layer (a declaration list, a rule, a declaration, component values) have a
 * byte form only as a stream: gtext_css_stream_new_bytes() takes any
 * GTEXT_CSS_Tree_Kind, decides the encoding the same way (a byte order mark,
 * the label, a leading `@charset` and the environment encoding apply to the
 * bytes whatever the entry point; the `@charset` is text of the input and is
 * parsed as that entry point parses it), and one feed of all the bytes is the
 * one-call parse of them.
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
 * @return GTEXT_CSS_OK, or GTEXT_CSS_E_INVALID for a NULL output, NULL
 *   bytes with a length, or an environment encoding that is not one.
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
 * is reached; its line and column are 0, because nothing was decoded. The
 * decoded text and its map have limits of their own (above), and a failure
 * that is not a limit (memory) has no place: `failure` is zero.
 *
 * @param bytes  The stylesheet; NULL only with `length` 0.
 * @param length Its length in bytes.
 * @param bytes_options NULL for none.
 * @param options NULL for the defaults.
 * @param out    Receives the tree, written only on success. Not NULL.
 * @param result NULL, or filled whatever happens (free it with
 *               gtext_css_result_free()); its `encoding`, `encoding_source`
 *               and `bom_length` say how the bytes were read (also for a
 *               failure once the encoding was decided, which every failure but
 *               the input limit and an invalid argument is).
 * @return As gtext_css_parse_stylesheet().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_parse_stylesheet_bytes(const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_DECODE_H
