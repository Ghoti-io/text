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
 * What the files of the CSS module share: the arena the tree lives in, the
 * token, the parse context, and the tokenizer's interface to the parser.
 *
 * css_core.c holds the statuses, names, options, the result and the error
 * list; css_tokenizer.c CSS Syntax section 4 (with the input stream's
 * preprocessing, 3.3); css_parser.c section 5 (every entry point, nesting);
 * css_anb.c section 6; css_serialize.c section 9 with CSSOM's idioms.
 *
 * Reference: CSS Syntax Module Level 3, Editor's Draft of 20 October 2026,
 * https://drafts.csswg.org/css-syntax/ (retrieved 2026-10-21).
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_INTERNAL_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The internal names below are not static, and not exported either: the
   library is built with -fvisibility=hidden and only GTEXT_API names are
   public, the convention html_* and markup_* follow. */

/* ---- the arena ------------------------------------------------------- */

/// One block of an arena.
typedef struct css_arena_chunk {
  struct css_arena_chunk * next; ///< The block before it.
  size_t used;                   ///< Bytes handed out.
  size_t cap;                    ///< Bytes after the header, which is
                                 ///< padded to 16.
} css_arena_chunk;

/// Memory that is handed out and never given back one piece at a time:
/// everything a tree holds is in one, and freeing the tree frees it.
typedef struct {
  GTEXT_Allocator alloc;  ///< What the blocks come from.
  css_arena_chunk * head; ///< The newest block.
  size_t total;           ///< Bytes of blocks, for the tests.
} css_arena;

/// Initialize an arena that allocates through `alloc`.
void css_arena_init(css_arena * arena, const GTEXT_Allocator * alloc);

/// `size` bytes aligned to 16, or NULL when the allocator refused.
void * css_arena_alloc(css_arena * arena, size_t size);

/// A copy of `len` bytes, NUL-terminated, or NULL when the allocator refused.
char * css_arena_strdup(css_arena * arena, const char * s, size_t len);

/// Free every block.
void css_arena_free(css_arena * arena);

/* ---- growable byte buffer --------------------------------------------- */

/// Scratch bytes: a string being built.
typedef struct {
  char * data; ///< The bytes.
  size_t len;  ///< In use.
  size_t cap;  ///< Allocated.
} css_buf;

/* ---- tokens ------------------------------------------------------------ */

/// A token, as the tokenizer produced it. Its strings are in the arena.
typedef struct {
  uint8_t type;            ///< GTEXT_CSS_Token_Type.
  uint8_t flags;           ///< GTEXT_CSS_FLAG_* bits.
  uint32_t delim;          ///< A delim's code point; a unicode-range's start.
  uint32_t range_end;      ///< A unicode-range's end.
  double number;           ///< A numeric token's value.
  size_t start;            ///< First byte.
  size_t end;              ///< One past the last.
  size_t line;             ///< Line of `start`.
  size_t column;           ///< Column of `start`.
  GTEXT_CSS_String value;  ///< The value, or a delim's UTF-8.
  GTEXT_CSS_String unit;   ///< A dimension's unit.
  GTEXT_CSS_String repr;   ///< A numeric token's number as written.
} css_tok;

/// A growable array of tokens.
typedef struct {
  css_tok * data; ///< The tokens.
  size_t len;     ///< In use.
  size_t cap;     ///< Allocated.
} css_tokvec;

/// One parse error as raised, before the list is sorted and cut.
typedef struct {
  GTEXT_CSS_Error_Code code; ///< Which.
  size_t offset;             ///< Where.
  size_t line;               ///< Line.
  size_t column;             ///< Column.
  size_t seq;                ///< The order it was raised in, for a stable sort.
} css_err;

/// A growable array of size_t.
typedef struct {
  size_t * data; ///< The values.
  size_t len;    ///< In use.
  size_t cap;    ///< Allocated.
} css_sizevec;

/* ---- where decoded text came from ------------------------------------ */

/// A run of code points of the decoded text that came from consecutive
/// stretches of the bytes: `count` code points, each `dlen` bytes of the
/// decoded text and `width` bytes of the source, the first at decoded offset
/// `dec` and source offset `src`.
typedef struct {
  size_t dec;     ///< Decoded offset of the first code point.
  size_t src;     ///< Source offset of the first code point.
  uint32_t count; ///< Code points in the run.
  uint8_t dlen;   ///< Bytes of each in the decoded text, 1 to 4.
  uint8_t width;  ///< Bytes of each in the source, 1 to 15.
} css_srcrun;

/**
 * The map from offsets in decoded text (what the tokenizer reads) back to
 * offsets in the bytes the caller gave, which is where every span and error
 * of a byte parse is reported.
 *
 * Text that came through a decoder has `runs`: a run is as long as the code
 * points stay in step (decoded and source lengths both uniform and the source
 * contiguous), so all-ASCII input in any ASCII-compatible encoding is one run,
 * and the map holds one entry for each change of the pattern. UTF-8 read in
 * place has no runs: its text is the bytes after a byte order mark, and an
 * offset is moved by `shift`.
 */
typedef struct css_srcmap {
  css_srcrun * runs;     ///< Sorted by `dec`; NULL for a shift.
  size_t nruns;          ///< In use.
  size_t cap;            ///< Allocated.
  size_t dec_total;      ///< Bytes of decoded text.
  size_t src_total;      ///< Bytes of source.
  size_t shift;          ///< Source offset of decoded offset 0, for a map with no runs.
  size_t * blocks;       ///< For each 4096 bytes of decoded text, the last run that starts at or
                         ///< before its first byte: a lookup bisects only the runs between this
                         ///< block's entry and the next one's, so it costs a constant however
                         ///< long the text is. NULL until the decoding has ended.
  size_t nblocks;        ///< Entries of `blocks`.
} css_srcmap;

/// Bytes of decoded text per entry of css_srcmap::blocks.
#define CSS_SRCMAP_BLOCK 4096u

/// The source offset where the code point that starts at decoded offset
/// `dec` was read. At or past the end: the length of the source.
size_t css_srcmap_start(const css_srcmap * map, size_t dec, size_t * work);

/// The source offset just after the code point that ends at decoded offset
/// `dec` (so after its last byte, and before any shift sequence that follows).
/// At the end of the text: the length of the source.
size_t css_srcmap_end(const css_srcmap * map, size_t dec, size_t * work);

/// Where the encoding of a byte parse came from, for the result.
typedef struct {
  GTEXT_Encoding encoding;                   ///< Used.
  GTEXT_CSS_Encoding_Source source;          ///< Why.
  size_t bom_length;                         ///< Bytes of byte order mark skipped.
} css_enc_info;

/// What a parse of decoded text is told about the bytes it came from.
typedef struct {
  const css_srcmap * map; ///< Decoded offsets to source offsets; not NULL.
  size_t source_length;   ///< The bytes' length (the input limit has been applied to it).
  css_enc_info encoding;  ///< For the result.
} css_srcinfo;

/// Everything a parse holds while it runs.
typedef struct {
  GTEXT_Allocator alloc;         ///< The caller's, or the default.

  /* Limits, with the defaults applied. */
  size_t max_input;              ///< Bytes.
  size_t max_tokens;             ///< Tokens.
  size_t max_depth;              ///< Nesting of blocks and functions.
  size_t max_rules;              ///< Rules made.
  size_t max_declarations;       ///< Declarations made.
  size_t max_total_errors;       ///< Parse errors raised.
  size_t max_errors;             ///< Parse errors kept.
  bool keep_errors;              ///< False with count_errors_only or no result.
  bool wtf8;                     ///< Read surrogates in WTF-8 as code points.
  GTEXT_CSS_Validity validity;   ///< "Valid in the current context".

  const unsigned char * in;      ///< The input.
  size_t in_len;                 ///< Its length.

  GTEXT_CSS_Status status;       ///< OK until something fails.
  GTEXT_CSS_Span failure;        ///< Where a limit was reached.

  css_arena arena;               ///< Where the tree goes.

  /* Parse errors. */
  css_err * errs;                ///< Raised, when kept.
  size_t nerr;                   ///< In `errs`.
  size_t errcap;                 ///< Allocated.
  size_t err_total;              ///< Raised in all.
  bool errors_on;                ///< False while a re-tokenization runs.
  size_t utf8_err_high;          ///< One past the last byte an invalid-utf-8 error covered.

  /* The token stream of the input. */
  css_tokvec toks;               ///< Every token.
  size_t * match;                ///< For an opening token, the index of its
                                 ///< closing token, or toks.len when the end
                                 ///< of the input closes it.
  css_tok eof;                   ///< The end-of-file token, at the end.
  size_t end_line;               ///< Line of the end of the input.
  size_t end_column;             ///< Column of the end of the input.

  css_buf sbuf;                  ///< Scratch for a token's string.
  css_buf ubuf;                  ///< Scratch for a dimension's unit.
  css_buf nbuf;                  ///< Scratch for a number's spelling.

  bool comma_seen;               ///< A comma-separated list consumed a comma.
  const css_srcmap * map;        ///< NULL, or where the text came from (a byte parse).

  size_t rule_count;             ///< Rules made.
  size_t declaration_count;      ///< Declarations made.
  size_t work;                   ///< Units of work, for the quadratic suite:
                                 ///< code points read and looked at, tokens
                                 ///< visited, values copied.
} css_ctx;

/// Mark the parse failed with `status` (the first failure wins).
void css_fail(css_ctx * ctx, GTEXT_CSS_Status status);

/// Mark the parse failed for a limit reached at a place.
void css_fail_at(css_ctx * ctx, GTEXT_CSS_Status status, size_t offset,
    size_t line, size_t column);

/// Raise a parse error (counted; kept when errors are kept; may fail the
/// parse for `max_total_errors`).
void css_error(css_ctx * ctx, GTEXT_CSS_Error_Code code, size_t offset,
    size_t line, size_t column);

/// Append to a byte buffer; false (and the context failed) on OOM.
bool css_buf_put(css_ctx * ctx, css_buf * buf, const char * s, size_t n);

/// Append a code point as UTF-8; anything that is not a scalar value (a
/// surrogate, above U+10FFFF) is appended as U+FFFD.
bool css_buf_put_cp(css_ctx * ctx, css_buf * buf, uint32_t cp);

/// Free a byte buffer.
void css_buf_free(css_ctx * ctx, css_buf * buf);

/// Append to a size_t vector.
bool css_sizevec_push(css_ctx * ctx, css_sizevec * vec, size_t value);

/**
 * Tokenize bytes `begin` to `end` of the input, starting at `line` and
 * `column`, appending to `out`. With `main` the tokens are the input's own:
 * `max_tokens` and `max_depth` are enforced and `ctx->match` is computed
 * (for `out` == &ctx->toks), and the end position is recorded. Without it
 * (a unicode-range value read again) no limit applies and no parse error is
 * raised. `eof` receives the end-of-file token of the range.
 *
 * @return false when the parse failed (OOM or a limit).
 */
bool css_tokenize(css_ctx * ctx, size_t begin, size_t end, size_t line,
    size_t column, bool unicode_ranges, bool main, css_tokvec * out,
    css_tok * eof);

/**
 * The line and column of byte `offset` of the input (clamped to its end),
 * counted exactly as the tokenizer counts them: one counter, so a failure's
 * position agrees with every token's. An offset inside a sequence is placed
 * at the end of that sequence's code point.
 */
void css_position_of(css_ctx * ctx, size_t offset, size_t * line, size_t * column);

/// Free a token vector's array (its strings are in the arena).
void css_tokvec_free(css_ctx * ctx, css_tokvec * vec);

/**
 * Copy bytes `begin` to `end` of the input with the input stream's
 * preprocessing applied (CR, CR LF and FF as LF; U+0000, surrogates and
 * ill-formed UTF-8 as U+FFFD): a custom property's original text.
 */
bool css_filtered_text(css_ctx * ctx, size_t begin, size_t end,
    GTEXT_CSS_String * out);

/**
 * The number a numeric token's spelling stands for: the math library's
 * correctly rounded decimal-to-double, which reads the spelling's `len` bytes
 * (no terminator is needed, and a spelling of any length is rounded exactly).
 *
 * `*flags` receives the conversion's GMATH_DECIMAL_* bits (inexact, overflow
 * to infinity, underflow below the smallest normal), which the tokenizer puts
 * on the token as GTEXT_CSS_FLAG_NUMBER_*.
 *
 * It cannot run out of memory. A spelling the math library does not read whole
 * is not a token's: the context is then failed with GTEXT_CSS_E_INTERNAL and
 * false is returned, with *out untouched.
 */
bool css_number_value(css_ctx * ctx, const char * repr, size_t len, double * out, unsigned * flags);

/// ASCII case-insensitive equality of a string with a lower-case literal.
bool css_ascii_ieq(const GTEXT_CSS_String * s, const char * lower);

/// The result's private part: the kept parse errors.
struct GTEXT_CSS_Result_Internal {
  GTEXT_Allocator allocator; ///< The one the block was made with.
  size_t count;              ///< Errors kept.
  size_t dropped;            ///< Errors raised and not kept.
  GTEXT_CSS_Error errors[];  ///< `count` of them.
};

/// The tree's private part: its memory.
struct GTEXT_CSS_Tree_Internal {
  css_arena arena; ///< Every node, string and array of the tree.
};

/// Fill a result from a finished context (status, counts, errors). Returns
/// the status (GTEXT_CSS_E_OOM if the error list could not be allocated). With
/// `ctx->map` the positions of the errors and of a failure are moved into the
/// source bytes.
GTEXT_CSS_Status css_finish_result(css_ctx * ctx, GTEXT_CSS_Result * result,
    size_t input_bytes);

/**
 * Parse text that was decoded from bytes. `text` is UTF-8 of `length` bytes,
 * `kind` names the entry point, and `info` says where the text came from: the
 * tree's spans and the result's positions are moved into the bytes, and the
 * result reports the encoding. The input limit is the caller's to apply to the
 * source (`info->source_length`); it is not applied to `length`. `work` (if
 * not NULL) receives the units of work (css_ctx::work).
 */
GTEXT_CSS_Status css_parse_decoded(GTEXT_CSS_Tree_Kind kind, const char * text,
    size_t length, const GTEXT_CSS_Parse_Options * options, const css_srcinfo * info,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result, size_t * work);

/// A result for a byte parse that was refused before any text existed (the
/// input is longer than the limit, memory ran out while reading): the status
/// and the byte offset of the failure, with the encoding if one was chosen.
void css_refused_result(const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Status status,
    size_t at, size_t line, size_t column, size_t input_bytes,
    const css_enc_info * encoding, GTEXT_CSS_Result * result);

/// The decoder of byte parses: UTF-8 text and the map of where it came from,
/// built as bytes arrive in pieces of any size.
typedef struct {
  GTEXT_Allocator alloc;  ///< Everything is allocated through this.
  struct GTEXT_Decoder * decoder; ///< The Encoding module's.
  char * text;            ///< The decoded text.
  size_t len;             ///< Its length.
  size_t cap;             ///< Allocated.
  css_srcmap map;         ///< Where each part came from.
  GTEXT_CSS_Status status; ///< OK, or OOM / INTERNAL once something failed.
  size_t work;            ///< Units of work: bytes taken, decoder steps, bytes of text and map made.
  size_t src_fed;         ///< Bytes of source given so far.
} css_decode;

/// Begin decoding `encoding` (with byte order mark sniffing, as the Encoding
/// Standard's "decode" has it). False (and `status`) on failure.
bool css_decode_begin(css_decode * d, const GTEXT_Allocator * alloc, GTEXT_Encoding encoding);

/// Decode `n` more bytes (and the end of the input if `final`).
bool css_decode_feed(css_decode * d, const unsigned char * bytes, size_t n, bool final);

/// Free what the decoder holds.
void css_decode_free(css_decode * d);


/// Set up a context from options.
void css_ctx_init(css_ctx * ctx, const GTEXT_CSS_Parse_Options * options,
    bool keep_errors);

/// Free what a context holds, the arena included unless `keep_arena`.
void css_ctx_free(css_ctx * ctx, bool keep_arena);

/**
 * The quadratic suite's view of a parse: what gtext_css_parse_<entry>() would
 * do, with the units of work it did (see css_ctx::work) in *work. `entry` is
 * a GTEXT_CSS_Tree_Kind. The tree, if any, is freed.
 */
GTEXT_INTERNAL_API GTEXT_CSS_Status gtext_css_parse_work(GTEXT_CSS_Tree_Kind entry,
    const char * input, size_t length, const GTEXT_CSS_Parse_Options * options,
    size_t * work);

/// The same for a parse of a stylesheet's bytes (gtext_css_parse_stylesheet_bytes() for
/// `kind` the stylesheet's): decoding and mapping included.
GTEXT_INTERNAL_API GTEXT_CSS_Status gtext_css_parse_bytes_work(GTEXT_CSS_Tree_Kind kind,
    const void * bytes, size_t length, const GTEXT_CSS_Bytes_Options * bytes_options,
    const GTEXT_CSS_Parse_Options * options, size_t * work);

/// The same for a stream: the stylesheet in `chunk`-byte pieces (the last one
/// shorter), the work of feeding and decoding included. `bytes_options` NULL
/// reads `data` as text. `chunk` 0 is one piece.
GTEXT_INTERNAL_API GTEXT_CSS_Status gtext_css_stream_work(GTEXT_CSS_Tree_Kind kind,
    const void * data, size_t length, size_t chunk, const GTEXT_CSS_Bytes_Options * bytes_options,
    const GTEXT_CSS_Parse_Options * options, size_t * work);

/// The tests' view of a stream's memory: the bytes it holds for the input (the
/// kept bytes, or the decoded text) and the bytes allocated for them.
GTEXT_INTERNAL_API size_t gtext_css_stream_kept_bytes(const GTEXT_CSS_Stream * stream, size_t * allocated);

/// The input limit of a parse with these options (the default if unset).
size_t css_max_input(const GTEXT_CSS_Parse_Options * options);

/// The same for the serializer: the units of work (bytes written, nodes
/// visited) of writing `tree` to `sink`.
GTEXT_INTERNAL_API GTEXT_CSS_Status gtext_css_serialize_work(const GTEXT_CSS_Tree * tree,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink,
    size_t * work);

/// The tests' view of css_buf_put_cp(): the UTF-8 of `cp` (U+FFFD for what is
/// not a scalar value) into `out` (at least 4 bytes), and its length; 0 if
/// the buffer could not be made.
GTEXT_INTERNAL_API size_t gtext_css_encode_cp(uint32_t cp, char * out);

/// Whether `cp` is a non-ASCII ident code point (CSS Syntax 4.2).
bool css_is_non_ascii_ident(uint32_t cp);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_INTERNAL_H
