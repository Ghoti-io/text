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
 * The HTML tokenizer: WHATWG HTML, "Tokenization" (section 13.2.5), as an
 * incremental, pull-driven component for the tree builder to drive.
 *
 * ## What it is
 *
 * The standard's state machine, every state of it, over a stream of bytes. The
 * input stream is preprocessed as the standard's "Preprocessing the input
 * stream" says: bytes are decoded as UTF-8 (an invalid sequence becomes U+FFFD,
 * once per maximal subpart, as the standard's UTF-8 decoder does), CR and CR LF
 * become LF, and surrogates, noncharacters and controls are reported as parse
 * errors. Nothing is guessed about other encodings; that is a later slice.
 *
 * ## How it is driven
 *
 * ```
 * tok = gtext_html_tokenizer_new(&options);
 * for each chunk:
 *   gtext_html_tokenizer_feed(tok, chunk, n);
 *   while ((s = gtext_html_tokenizer_next(tok, &token)) == TOKEN) { ... }
 *   // s is NEED_INPUT here: the chunk may be released
 * gtext_html_tokenizer_finish(tok);
 * while ((s = gtext_html_tokenizer_next(tok, &token)) == TOKEN) { ... }
 * // s is DONE: the end-of-file token was the last one delivered
 * ```
 *
 * **A token is delivered the moment it is complete, and the tokenizer has
 * consumed nothing past it.** That is the contract the tree builder needs: it
 * handles a start tag, calls gtext_html_tokenizer_set_state() (or leaves the
 * state alone), and the very next character is read in the state it chose.
 * Character tokens are the one exception to "the moment": a run of characters
 * is held until something that is not a character ends it (or the input ends),
 * so that runs are coalesced the same way however the input was cut. A run is
 * held in memory, so a run longer than `max_text_bytes` is a hard failure
 * (E_LIMIT_TEXT), not a split into two tokens; the default for it equals the
 * default for the input (64 MiB), so by default the input limit comes first.
 *
 * **Memory.** A chunk passed to feed() is *borrowed*, not copied: it must stay
 * valid until next() returns NEED_INPUT (or the tokenizer is freed), at which
 * point the few bytes that could still change meaning - the front of a UTF-8
 * sequence, a CR waiting for its LF, the look-ahead of a named character
 * reference - have been copied into the tokenizer. When such a tail is waiting
 * the next chunk is read in place after at most 256 bytes of it have been joined to
 * the tail (and then the rest in place again), so what the tokenizer copies is bounded
 * by the tail and that window and not by the chunk. Strings in a token point
 * into the tokenizer and are valid until the next call to next(). Memory otherwise is
 * the construct being built: a name, a value, a comment, or the run of characters held
 * for coalescing.
 *
 * ## Chunking
 *
 * The token stream and the error list are the same for any way of cutting the
 * input, a byte at a time included, **with one exception: `max_input_bytes`**. That limit is on
 * what has been *fed*, so a whole input over it is refused by feed() before any token, and the
 * same input cut up delivers the tokens before the byte that crosses it and then the same
 * status. (A content limit - a name, a value, a text run - is reached at the same byte of the
 * input however it was cut, but when both limits would be reached, which status comes first can
 * depend on the cutting; either way it is a limit status.) The machine is suspended only *between*
 * characters (a character is decoded whole or not at all), the places that
 * look ahead - `<!--`, `DOCTYPE`, `[CDATA[`, `PUBLIC`, `SYSTEM`, and the longest
 * named character reference - decide as soon as the characters seen so far
 * decide the question and otherwise wait for more, and every wait is bounded by
 * the length of what is being looked for. Work is linear in the input: a
 * comment that never ends costs a bounded amount per byte, not a rescan.
 *
 * ## Errors
 *
 * Parse errors are collected, never failures. Each has the standard's code,
 * the line and column (1-based, in characters after newline normalization) of
 * the character the standard calls the current input character, and its byte
 * offset in the input as fed (a CR LF pair is at the CR). Only the first
 * `max_errors` are kept; the rest are counted. The one way tokenizing fails is
 * a limit (or the allocator): a distinct status, sticky, with the errors seen
 * so far still readable.
 *
 * ## What it does not do
 *
 * Tree construction, and so no "adjusted current node": the tree builder says
 * whether a CDATA section is allowed (see GTEXT_HTML_TokenizerOptions) and sets
 * the state after a start tag. Encoding sniffing and transcoding (3f). Scripts.
 *
 * Internal. Nothing here is public API until the HTML parser is.
 */

#ifndef GHOTI_IO_GTEXT_SRC_HTML_HTML_TOKENIZER_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_HTML_HTML_TOKENIZER_INTERNAL_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A tokenizer. Opaque.
typedef struct GTEXT_HTML_Tokenizer GTEXT_HTML_Tokenizer;

/* ---- Defaults ----------------------------------------------------------- */

#define GTEXT_HTML_TOKENIZER_DEFAULT_MAX_INPUT ((size_t)64 * 1024 * 1024)
#define GTEXT_HTML_TOKENIZER_DEFAULT_MAX_NAME ((size_t)8192)
#define GTEXT_HTML_TOKENIZER_DEFAULT_MAX_VALUE ((size_t)16 * 1024 * 1024)
#define GTEXT_HTML_TOKENIZER_DEFAULT_MAX_COMMENT ((size_t)16 * 1024 * 1024)
#define GTEXT_HTML_TOKENIZER_DEFAULT_MAX_TEXT ((size_t)64 * 1024 * 1024)
#define GTEXT_HTML_TOKENIZER_DEFAULT_MAX_ATTRIBUTES ((size_t)1024)
#define GTEXT_HTML_TOKENIZER_DEFAULT_MAX_ERRORS ((size_t)1024)

/// The scratch the named character reference state looks names up in: more than the table's
/// longest name (32 bytes, `;` included). A test checks that.
#define GTEXT_HTML_TOKENIZER_NAME_WINDOW 40

/// The most work (see gtext_html_tokenizer_work()) the tokenizer does for `n` bytes of input fed
/// a byte at a time: the one bound the tests, the fuzzer and the documentation hold it to.
#define GTEXT_HTML_TOKENIZER_WORK_BOUND(n) (48 * (size_t)(n) + 4096)

/* ---- Parse errors ------------------------------------------------------- */

/**
 * @brief The parse errors, by the standard's names ("Parse errors", 13.2.2).
 *
 * `gtext_html_error_code_name()` gives the spelling the standard uses
 * (`abrupt-closing-of-empty-comment`). 52 of them are the standard's list: the
 * tokenizer raises 51, and NON_VOID_HTML_ELEMENT_START_TAG_WITH_TRAILING_SOLIDUS
 * is declared for the tree builder (a self-closing flag nobody acknowledged),
 * so that its list and this one are one list. INVALID_UTF8 is the 53rd and is not the
 * standard's: it is the decoding step's error, which the standard leaves
 * without a name.
 */
typedef enum {
  GTEXT_HTML_ERR_ABRUPT_CLOSING_OF_EMPTY_COMMENT = 0,
  GTEXT_HTML_ERR_ABRUPT_DOCTYPE_PUBLIC_IDENTIFIER,
  GTEXT_HTML_ERR_ABRUPT_DOCTYPE_SYSTEM_IDENTIFIER,
  GTEXT_HTML_ERR_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE,
  GTEXT_HTML_ERR_CDATA_IN_HTML_CONTENT,
  GTEXT_HTML_ERR_CHARACTER_REFERENCE_OUTSIDE_UNICODE_RANGE,
  GTEXT_HTML_ERR_CONTROL_CHARACTER_IN_INPUT_STREAM,
  GTEXT_HTML_ERR_CONTROL_CHARACTER_REFERENCE,
  GTEXT_HTML_ERR_DISALLOWED_PROCESSING_INSTRUCTION_TARGET,
  GTEXT_HTML_ERR_DUPLICATE_ATTRIBUTE,
  GTEXT_HTML_ERR_END_TAG_WITH_ATTRIBUTES,
  GTEXT_HTML_ERR_END_TAG_WITH_TRAILING_SOLIDUS,
  GTEXT_HTML_ERR_EOF_BEFORE_TAG_NAME,
  GTEXT_HTML_ERR_EOF_IN_CDATA,
  GTEXT_HTML_ERR_EOF_IN_COMMENT,
  GTEXT_HTML_ERR_EOF_IN_DOCTYPE,
  GTEXT_HTML_ERR_EOF_IN_PROCESSING_INSTRUCTION,
  GTEXT_HTML_ERR_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT,
  GTEXT_HTML_ERR_EOF_IN_TAG,
  GTEXT_HTML_ERR_INCORRECTLY_CLOSED_COMMENT,
  GTEXT_HTML_ERR_INCORRECTLY_OPENED_COMMENT,
  GTEXT_HTML_ERR_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME,
  GTEXT_HTML_ERR_INVALID_FIRST_CHARACTER_OF_PROCESSING_INSTRUCTION_TARGET,
  GTEXT_HTML_ERR_INVALID_FIRST_CHARACTER_OF_TAG_NAME,
  GTEXT_HTML_ERR_INVALID_PROCESSING_INSTRUCTION_TARGET,
  GTEXT_HTML_ERR_MISSING_ATTRIBUTE_VALUE,
  GTEXT_HTML_ERR_MISSING_DOCTYPE_NAME,
  GTEXT_HTML_ERR_MISSING_DOCTYPE_PUBLIC_IDENTIFIER,
  GTEXT_HTML_ERR_MISSING_DOCTYPE_SYSTEM_IDENTIFIER,
  GTEXT_HTML_ERR_MISSING_END_TAG_NAME,
  GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER,
  GTEXT_HTML_ERR_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER,
  GTEXT_HTML_ERR_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE,
  GTEXT_HTML_ERR_MISSING_WHITESPACE_AFTER_DOCTYPE_PUBLIC_KEYWORD,
  GTEXT_HTML_ERR_MISSING_WHITESPACE_AFTER_DOCTYPE_SYSTEM_KEYWORD,
  GTEXT_HTML_ERR_MISSING_WHITESPACE_BEFORE_DOCTYPE_NAME,
  GTEXT_HTML_ERR_MISSING_WHITESPACE_BETWEEN_ATTRIBUTES,
  GTEXT_HTML_ERR_MISSING_WHITESPACE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS,
  GTEXT_HTML_ERR_NESTED_COMMENT,
  GTEXT_HTML_ERR_NONCHARACTER_CHARACTER_REFERENCE,
  GTEXT_HTML_ERR_NONCHARACTER_IN_INPUT_STREAM,
  GTEXT_HTML_ERR_NON_VOID_HTML_ELEMENT_START_TAG_WITH_TRAILING_SOLIDUS,
  GTEXT_HTML_ERR_NULL_CHARACTER_REFERENCE,
  GTEXT_HTML_ERR_SURROGATE_CHARACTER_REFERENCE,
  GTEXT_HTML_ERR_SURROGATE_IN_INPUT_STREAM,
  GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_AFTER_DOCTYPE_SYSTEM_IDENTIFIER,
  GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_IN_ATTRIBUTE_NAME,
  GTEXT_HTML_ERR_UNEXPECTED_CHARACTER_IN_UNQUOTED_ATTRIBUTE_VALUE,
  GTEXT_HTML_ERR_UNEXPECTED_EQUALS_SIGN_BEFORE_ATTRIBUTE_NAME,
  GTEXT_HTML_ERR_UNEXPECTED_NULL_CHARACTER,
  GTEXT_HTML_ERR_UNEXPECTED_SOLIDUS_IN_TAG,
  GTEXT_HTML_ERR_UNKNOWN_NAMED_CHARACTER_REFERENCE,
  /// Not the standard's: a byte sequence the UTF-8 decoder replaced.
  GTEXT_HTML_ERR_INVALID_UTF8,
  GTEXT_HTML_ERR_COUNT
} GTEXT_HTML_ErrorCode;

/// One parse error.
typedef struct {
  GTEXT_HTML_ErrorCode code; ///< Which.
  size_t line;               ///< 1-based.
  size_t column;             ///< 1-based, in characters.
  size_t offset;             ///< Byte offset in the input as fed.
} GTEXT_HTML_ParseError;

/**
 * @brief The standard's spelling of an error code.
 *
 * @param code A code.
 * @return A static string, or "unknown" for a value outside the enum.
 */
GTEXT_INTERNAL_API const char * gtext_html_error_code_name(
    GTEXT_HTML_ErrorCode code);

/* ---- Tokens ------------------------------------------------------------- */

/// What a token is.
typedef enum {
  GTEXT_HTML_TOKEN_DOCTYPE = 1,
  GTEXT_HTML_TOKEN_START_TAG,
  GTEXT_HTML_TOKEN_END_TAG,
  GTEXT_HTML_TOKEN_COMMENT,
  GTEXT_HTML_TOKEN_CHARACTER, ///< A coalesced run.
  GTEXT_HTML_TOKEN_PROCESSING_INSTRUCTION,
  GTEXT_HTML_TOKEN_EOF
} GTEXT_HTML_TokenType;

/// An attribute of a tag, in source order, duplicates already removed.
typedef struct {
  const char * name;   ///< Lower-cased, UTF-8, not terminated.
  size_t name_length;
  const char * value;  ///< References decoded, UTF-8, not terminated.
  size_t value_length;
} GTEXT_HTML_Attribute;

/**
 * @brief A token. Valid until the next call to gtext_html_tokenizer_next().
 *
 * Which fields mean something depends on `type`:
 *
 * | type | fields |
 * | --- | --- |
 * | DOCTYPE | `has_name`/`name`, `has_public_id`/`public_id`, `has_system_id`/`system_id`, `force_quirks` |
 * | START_TAG | `name`, `attributes`, `self_closing` |
 * | END_TAG | `name`, `attributes` (the standard reports them as an error and the tree builder ignores them), `self_closing` |
 * | COMMENT | `data` |
 * | CHARACTER | `data`, `all_whitespace`, `has_null` |
 * | PROCESSING_INSTRUCTION | `name` (the target), `data` |
 * | EOF | nothing |
 *
 * Strings are UTF-8 and are not NUL-terminated; a missing string has a NULL
 * pointer and length 0, and an empty one a non-NULL pointer and length 0.
 * Lone surrogates, which only a tokenizer in `wtf8` mode can see, are encoded
 * as three bytes each.
 */
typedef struct {
  GTEXT_HTML_TokenType type;
  const char * name;
  size_t name_length;
  const char * data;
  size_t data_length;
  const GTEXT_HTML_Attribute * attributes;
  size_t attribute_count;
  const char * public_id;
  size_t public_id_length;
  const char * system_id;
  size_t system_id_length;
  bool has_name;
  bool has_public_id;
  bool has_system_id;
  bool force_quirks;
  bool self_closing;
  /// CHARACTER: every character is U+0009, U+000A, U+000C or U+0020.
  bool all_whitespace;
  /// CHARACTER: the run holds a U+0000 (data and CDATA states emit it as is;
  /// the tree builder decides what it means).
  bool has_null;
  size_t line;   ///< Where the token starts (the `<` of a tag), 1-based.
  size_t column; ///< 1-based, in characters.
  size_t offset; ///< Byte offset in the input as fed.
} GTEXT_HTML_Token;

/* ---- States the tree builder can set ------------------------------------ */

/**
 * @brief The tokenizer states a tree builder (or a test) can switch to.
 *
 * "Switch the tokenizer to the RCDATA state" and the rest of the tree
 * construction stage's instructions. `CDATA_SECTION` is for the test vectors,
 * which start in it; a tree builder reaches it through allow_cdata.
 */
typedef enum {
  GTEXT_HTML_STATE_DATA = 0,
  GTEXT_HTML_STATE_RCDATA,
  GTEXT_HTML_STATE_RAWTEXT,
  GTEXT_HTML_STATE_SCRIPT_DATA,
  GTEXT_HTML_STATE_PLAINTEXT,
  GTEXT_HTML_STATE_CDATA_SECTION
} GTEXT_HTML_TokenizerState;

/* ---- Status and options ------------------------------------------------- */

/// What a call to the tokenizer returned.
typedef enum {
  GTEXT_HTML_TOKENIZER_TOKEN = 0, ///< next(): a token was produced.
  GTEXT_HTML_TOKENIZER_NEED_INPUT, ///< next(): feed more, or finish().
  GTEXT_HTML_TOKENIZER_DONE,       ///< next(): the EOF token was delivered.
  GTEXT_HTML_TOKENIZER_OK,         ///< feed(): taken.
  GTEXT_HTML_TOKENIZER_E_INVALID,  ///< A NULL where a value is needed.
  GTEXT_HTML_TOKENIZER_E_STATE,    ///< feed() with input unconsumed, or after finish().
  GTEXT_HTML_TOKENIZER_E_OOM,      ///< The allocator returned NULL.
  GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT,      ///< More than `max_input_bytes` fed.
  GTEXT_HTML_TOKENIZER_E_LIMIT_NAME,       ///< A name longer than `max_name_bytes`.
  GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE,      ///< A value longer than `max_value_bytes`.
  GTEXT_HTML_TOKENIZER_E_LIMIT_COMMENT,    ///< A comment longer than `max_comment_bytes`.
  GTEXT_HTML_TOKENIZER_E_LIMIT_TEXT,       ///< A text run longer than `max_text_bytes`.
  GTEXT_HTML_TOKENIZER_E_LIMIT_ATTRIBUTES  ///< More than `max_attributes` on a tag.
} GTEXT_HTML_TokenizerStatus;

/// Whether `status` is one of the limit statuses.
#define GTEXT_HTML_TOKENIZER_IS_LIMIT(status) \
  ((status) >= GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT)

/**
 * @brief Options for gtext_html_tokenizer_new().
 *
 * Every limit is a size, and 0 selects its default; 0 never means unlimited
 * (SIZE_MAX is as close as the type allows). Zero-initialising the struct
 * gives the defaults.
 */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the default.
  size_t max_input_bytes;   ///< Total bytes fed.
  size_t max_name_bytes;    ///< A tag, attribute or DOCTYPE name; a PI target.
  size_t max_value_bytes;   ///< An attribute value; a DOCTYPE identifier.
  size_t max_comment_bytes; ///< A comment's data; a PI's data.
  size_t max_text_bytes;    ///< One coalesced run of characters.
  size_t max_attributes;    ///< Attributes kept on one tag.
  size_t max_errors;        ///< Parse errors kept; the rest are counted. Not a failure.
  /// Keep no errors at all and only count them (gtext_html_tokenizer_errors_dropped()):
  /// `max_errors` cannot say 0, because 0 there means the default.
  bool count_errors_only;
  /// Seed of the hash that finds repeated attribute names. 0 (the default) gives each
  /// tokenizer a seed of its own, from a process-wide counter: the same on every run of a
  /// single-threaded program, different for each tokenizer. Tests set it to be exact.
  uint64_t hash_seed;
  /**
   * Asked at `<![CDATA[`: is there an adjusted current node that is not in the
   * HTML namespace? NULL means no (the HTML parser's answer outside foreign
   * content). See also gtext_html_tokenizer_set_allow_cdata().
   */
  bool (*allow_cdata)(void * user);
  void * allow_cdata_user;
  /**
   * Treat the input as WTF-8: the three-byte forms of lone surrogates are read
   * as the surrogates they spell (a surrogate-in-input-stream error each)
   * instead of being replaced. UTF-8 cannot carry them; the html5lib vectors
   * do, as JSON escapes.
   */
  bool wtf8;
} GTEXT_HTML_TokenizerOptions;

/* ---- Lifecycle ---------------------------------------------------------- */

/**
 * @brief Make a tokenizer, in the data state.
 *
 * @param options Options, or NULL for the defaults. Copied.
 * @return The tokenizer, or NULL if the allocator failed.
 */
GTEXT_INTERNAL_API GTEXT_HTML_Tokenizer * gtext_html_tokenizer_new(
    const GTEXT_HTML_TokenizerOptions * options);

/**
 * @brief Free a tokenizer and everything it holds. NULL is ignored.
 */
GTEXT_INTERNAL_API void gtext_html_tokenizer_free(GTEXT_HTML_Tokenizer * tok);

/**
 * @brief Hand the tokenizer some input.
 *
 * The bytes are borrowed: they must stay valid until next() returns
 * NEED_INPUT. Feeding is refused (E_STATE) while the previous chunk has not
 * been drained that way, or after finish().
 *
 * @param tok The tokenizer.
 * @param data The bytes. May be NULL if `length` is 0.
 * @param length How many.
 * @return OK, E_STATE, E_LIMIT_INPUT or E_INVALID.
 */
GTEXT_INTERNAL_API GTEXT_HTML_TokenizerStatus gtext_html_tokenizer_feed(
    GTEXT_HTML_Tokenizer * tok, const void * data, size_t length);

/**
 * @brief Say that there is no more input.
 *
 * After this, next() eventually produces the EOF token and then DONE.
 */
GTEXT_INTERNAL_API void gtext_html_tokenizer_finish(GTEXT_HTML_Tokenizer * tok);

/**
 * @brief Run the machine until it has a token to deliver.
 *
 * @param tok The tokenizer.
 * @param token Where to put a pointer to the token on TOKEN; valid until the
 *   next call. Not written otherwise.
 * @return TOKEN, NEED_INPUT, DONE, or a failure status (sticky: the same one
 *   again on every later call).
 */
GTEXT_INTERNAL_API GTEXT_HTML_TokenizerStatus gtext_html_tokenizer_next(
    GTEXT_HTML_Tokenizer * tok, const GTEXT_HTML_Token ** token);

/* ---- For the tree builder ----------------------------------------------- */

/**
 * @brief Switch the state machine.
 *
 * Call it between tokens: the state the tokenizer is in when next() returns a
 * token is the one the standard's next character is read in, so a state set
 * then is the state "after" that token.
 *
 * @return OK, or E_INVALID for a value outside the enum.
 */
GTEXT_INTERNAL_API GTEXT_HTML_TokenizerStatus gtext_html_tokenizer_set_state(
    GTEXT_HTML_Tokenizer * tok, GTEXT_HTML_TokenizerState state);

/// The state the machine is in now, if it is one set_state() can set; the data
/// state for any other (it is in the middle of a construct).
GTEXT_INTERNAL_API GTEXT_HTML_TokenizerState gtext_html_tokenizer_get_state(
    const GTEXT_HTML_Tokenizer * tok);

/**
 * @brief Set "the last start tag emitted from this tokenizer".
 *
 * For the appropriate-end-tag test, when a tokenizer is created in the middle
 * of a document (the fragment case) or by a test. The tokenizer keeps it up to
 * date itself from then on. A NULL name clears it ("no start tag has been
 * emitted").
 *
 * @return OK, E_OOM, or E_LIMIT_NAME.
 */
GTEXT_INTERNAL_API GTEXT_HTML_TokenizerStatus
gtext_html_tokenizer_set_last_start_tag(
    GTEXT_HTML_Tokenizer * tok, const char * name, size_t length);

/// What answers "is a CDATA section allowed here?".
typedef enum {
  GTEXT_HTML_CDATA_DEFAULT = 0, ///< The `allow_cdata` callback (no callback: not allowed).
  GTEXT_HTML_CDATA_ALLOW,       ///< Allowed, whatever the callback says.
  GTEXT_HTML_CDATA_REFUSE       ///< Not allowed, whatever the callback says.
} GTEXT_HTML_CdataOverride;

/// Set the answer, from the tree builder's token handler. It stands until it is called again;
/// GTEXT_HTML_CDATA_DEFAULT returns the decision to the option's callback.
GTEXT_INTERNAL_API void gtext_html_tokenizer_set_allow_cdata(
    GTEXT_HTML_Tokenizer * tok, GTEXT_HTML_CdataOverride mode);

/* ---- Errors and tests --------------------------------------------------- */

/// How many parse errors are kept (0 for a NULL tokenizer, as for the three below).
GTEXT_INTERNAL_API size_t gtext_html_tokenizer_error_count(
    const GTEXT_HTML_Tokenizer * tok);

/// The i-th kept error (in the order raised), or NULL if there is none.
GTEXT_INTERNAL_API const GTEXT_HTML_ParseError * gtext_html_tokenizer_error_at(
    const GTEXT_HTML_Tokenizer * tok, size_t index);

/// How many parse errors were raised and not kept, because of `max_errors`.
GTEXT_INTERNAL_API size_t gtext_html_tokenizer_errors_dropped(
    const GTEXT_HTML_Tokenizer * tok);

/// Tests: the attribute-name hash, so that a test can build names that collide under a seed.
GTEXT_INTERNAL_API uint64_t gtext_html_tokenizer_name_hash(
    uint64_t seed, const char * s, size_t n);

/// Tests: units of work done so far (characters decoded, look-ahead steps,
/// bytes copied). Linear in the input however it was fed; a test holds it to a
/// multiple of the input length.
GTEXT_INTERNAL_API size_t gtext_html_tokenizer_work(
    const GTEXT_HTML_Tokenizer * tok);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_HTML_HTML_TOKENIZER_INTERNAL_H
