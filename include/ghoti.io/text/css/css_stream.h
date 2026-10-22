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
 * @file css_stream.h
 * @brief An incremental CSS parser: the same answer at every cut.
 *
 * A stylesheet that arrives in pieces (a network response, a file read in
 * blocks) is given to a stream with gtext_css_stream_feed(), and
 * gtext_css_stream_finish() returns the tree, as the parse of the whole input
 * would. **The tree, the parse errors and the verdict do not depend on where
 * the input was cut**: not between two bytes of an escape, a comment, a
 * `url()`, a multi-byte character or a byte order mark, not at a piece of one
 * byte or of none, and not in the first kilobyte that decides the encoding.
 * The tests cut every input at every point, a byte at a time, and at random,
 * and compare the result with the one-call parse.
 *
 * ## What is kept, and what it costs
 *
 * CSS is parsed whole: its tokenizer matches brackets across the entire
 * input and the grammar of a rule is not known until its block ends, so a
 * stream **buffers the stylesheet** (the input, or for an encoding other than
 * UTF-8 its decoded text and a map of where it came from) and parses it at
 * the end. That buffer is bounded: `max_input_bytes` applies to the bytes fed,
 * and the feed that goes past it fails with GTEXT_CSS_E_LIMIT_INPUT before the
 * piece is kept. The buffer grows by doubling, so feeding costs time in
 * proportion to the bytes and never to the number of calls; calls with no
 * bytes cost nothing. The parse itself is iterative and bounded by the same
 * limits as the one-call functions.
 *
 * The memory is that of the one-call function's decode (css_decode.h, "What is
 * read in place, and what the rest costs"): for an encoding other than UTF-8,
 * the decoded text (at most `max_decoded_bytes`) and its map (at most
 * `max_map_runs` runs of 24 bytes), about 27 bytes for each byte fed in the
 * worst case, and the same limit status, GTEXT_CSS_E_LIMIT_DECODED.
 *
 * ## Failure, and the life of a stream
 *
 * Until gtext_css_stream_finish() is called, a failure of the stream (a limit,
 * memory) is returned by the call that met it and by every feed after it, the
 * same status each time (it is sticky); it is also the status finish returns,
 * once, with the result filled in. **After finish, every call but
 * gtext_css_stream_free() returns GTEXT_CSS_E_INVALID**: a feed, and a second
 * finish, which does not repeat the failure and does not fill the result again
 * (a result it is given is set to GTEXT_CSS_E_INVALID). There is no status for a
 * syntax error, because CSS has none.
 *
 * The result of a failed finish is the one-call function's for the same
 * failure: a limit has a place (the byte; in text input the line and column),
 * a failed allocation has none (`failure` is zero), and once the encoding of a
 * byte stream was decided the result says so, except for the input limit.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_STREAM_H
#define GHOTI_IO_GTEXT_CSS_CSS_STREAM_H

#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_decode.h>
#include <ghoti.io/text/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An incremental parser. Made by gtext_css_stream_new() or
 *  gtext_css_stream_new_bytes(), freed by gtext_css_stream_free(). */
typedef struct GTEXT_CSS_Stream GTEXT_CSS_Stream;

/**
 * @brief Begin an incremental parse of text that is already decoded (UTF-8).
 *
 * The result is that of the matching one-call function for `kind` (for
 * GTEXT_CSS_TREE_STYLESHEET, gtext_css_parse_stylesheet()). A byte order mark
 * is not looked for.
 *
 * @param kind    Which entry point's tree to build: any GTEXT_CSS_Tree_Kind;
 *                a value that is not one is GTEXT_CSS_E_INVALID.
 * @param options NULL for the defaults. Copied (the allocator's struct and the
 *                `location` string too); the allocator's `ctx` and the
 *                validity callbacks' `user` must outlive the stream.
 * @param out     Receives the stream. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID for a NULL output or a bad kind;
 *   GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_stream_new(GTEXT_CSS_Tree_Kind kind,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Stream ** out);

/**
 * @brief Begin an incremental parse of a stylesheet's bytes (CSS Syntax 3.2).
 *
 * The encoding is decided from the first GTEXT_CSS_ENCODING_WINDOW bytes (or
 * all of them, if there are fewer), as gtext_css_determine_encoding() does; the
 * result of gtext_css_stream_finish() is that of
 * gtext_css_parse_stylesheet_bytes() for `kind` GTEXT_CSS_TREE_STYLESHEET. For
 * another kind the bytes are decoded the same way (a byte order mark, the
 * label, a leading `@charset` and the environment encoding decide the encoding
 * whatever the kind, and the `@charset` text is parsed as that entry point
 * parses it) and the result is that entry point's on the decoded text, with
 * every position in the bytes. There is no one-call byte function for those
 * kinds: one feed of all the bytes is it.
 *
 * @param kind    Which entry point's tree to build; a value that is not a
 *                GTEXT_CSS_Tree_Kind is GTEXT_CSS_E_INVALID.
 * @param bytes_options NULL for none. Copied, the label too.
 * @param options NULL for the defaults. Copied as for gtext_css_stream_new().
 * @param out     Receives the stream. Not NULL.
 * @return As gtext_css_stream_new().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_stream_new_bytes(GTEXT_CSS_Tree_Kind kind,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Stream ** out);

/**
 * @brief Give a stream the next piece of the input.
 *
 * @param stream A stream.
 * @param data   The piece, copied before the call returns; NULL only with a
 *               `length` of 0.
 * @param length Its length, which may be 0.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID for a NULL stream, NULL data with
 *   a length, or a stream already finished (a failed one too: finished wins);
 *   GTEXT_CSS_E_LIMIT_INPUT when the input has gone past `max_input_bytes`;
 *   GTEXT_CSS_E_LIMIT_DECODED; GTEXT_CSS_E_OOM. The last three are the
 *   stream's failure, and are returned by every feed until finish.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_stream_feed(GTEXT_CSS_Stream * stream,
    const void * data, size_t length);

/**
 * @brief End the input and take the tree.
 *
 * The stream is finished whatever the result; feed and a second finish are
 * refused after it with GTEXT_CSS_E_INVALID (even when the stream had failed),
 * and gtext_css_stream_free() still has to be called.
 *
 * @param stream A stream.
 * @param out    Receives the tree, written only on success. Not NULL.
 * @param result NULL, or filled whatever happens, as for the one-call
 *               functions; after a failed feed it has the failure's status
 *               and place, and `input_bytes` counts the bytes fed up to and
 *               including the piece that went past the limit.
 * @return The status of the parse, or the stream's failure.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_stream_finish(GTEXT_CSS_Stream * stream,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result);

/**
 * @brief Release a stream and everything it holds, finished or not.
 *
 * @param stream A stream; NULL is ignored. A tree gtext_css_stream_finish()
 *               returned is the caller's and outlives it.
 */
GTEXT_API void gtext_css_stream_free(GTEXT_CSS_Stream * stream);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_STREAM_H
