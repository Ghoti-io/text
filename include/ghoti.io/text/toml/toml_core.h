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
 * @file toml_core.h
 * @brief Status codes, error payload and parse options for the TOML module.
 *
 * The default specification is **TOML v1.0.0, 2021-01-12**. The 1.1.0 preview
 * is available through GTEXT_TOML_Parse_Options::version, and is an option
 * rather than a relaxation of the default because 1.1.0 is not 1.0.0 plus
 * permissions - it also *refuses* things 1.0.0's prose allows.
 *
 * TOML's four date-time types are `chron`'s. This module does not carry a
 * second date-time grammar: a scanned date-time is handed to
 * gchron_parse_toml() and what comes back is stored as the GCHRON_TomlValue it
 * is. See @ref format_toml for the clause-by-clause account.
 */

#ifndef GHOTI_IO_GTEXT_TOML_TOML_CORE_H
#define GHOTI_IO_GTEXT_TOML_TOML_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_TOML_Status
 * @brief Status codes returned by TOML APIs.
 *
 * Every refusal this module makes is one of these, and the format page names
 * which malformation produces which. The list is longer than a generic
 * "invalid" because TOML's invalid cases fall into groups a caller can act on
 * differently: a duplicate key is a mistake in the document's data, a
 * redefinition is a mistake in its structure, and a control character is a
 * mistake in its encoding.
 */
typedef enum {
  GTEXT_TOML_OK = 0,      ///< Success.
  GTEXT_TOML_E_INVALID,   ///< A parameter was not usable (NULL, absurd length).
  GTEXT_TOML_E_OOM,       ///< Out of memory.
  GTEXT_TOML_E_LIMIT,     ///< A configured limit was exceeded.
  GTEXT_TOML_E_DEPTH,     ///< Maximum nesting depth exceeded.
  GTEXT_TOML_E_BAD_TOKEN, ///< Syntax: something is not where the grammar
                          ///< allows it.
  GTEXT_TOML_E_BAD_ESCAPE,  ///< An escape sequence no TOML version defines.
  GTEXT_TOML_E_BAD_UNICODE, ///< Invalid UTF-8, or an escape naming a scalar
                            ///< value that is not one (a surrogate, or beyond
                            ///< U+10FFFF).
  GTEXT_TOML_E_CONTROL,     ///< A control character where the specification
                            ///< forbids one - in a string, a comment or a key.
  GTEXT_TOML_E_RANGE,       ///< A number outside what its type can hold: an
                            ///< integer outside int64_t.
  GTEXT_TOML_E_DUPKEY,      ///< A key defined twice. Not an option: TOML says
                            ///< this is invalid, where JSON's RFC says names
                            ///< SHOULD be unique and leaves it open.
  GTEXT_TOML_E_REDEFINE,    ///< A table or array conflict: a header repeated, a
                            ///< table extended after being written inline, or
                            ///< `[[a]]` against a statically defined `a`.
  GTEXT_TOML_E_DATETIME,    ///< A date-time `chron` refused - an impossible
                            ///< date, an out-of-range offset.
  GTEXT_TOML_E_WRITE,       ///< A sink refused output during serialization.
  GTEXT_TOML_E_STATE,       ///< Operation not valid in the current state.
  /**
   * The value is fine and this format has no spelling for it.
   *
   * Not a malformed document, which is what every other code above reports,
   * and the distinction is one a caller acts on: a batch converter retries
   * nothing on a syntax error and may well want to re-run this one with a
   * different policy. It is reported by the two conversions - a JSON `null`
   * where TOML has no null, a TOML `nan` where JSON has no non-finite number -
   * and by the writer, for a comment on a value inside `{ }` where a comment
   * cannot go.
   */
  GTEXT_TOML_E_UNREPRESENTABLE
} GTEXT_TOML_Status;

/**
 * @struct GTEXT_TOML_Error
 * @brief Where a parse failed and what it was looking at.
 *
 * `offset` counts bytes of the input buffer. Unlike the YAML module there is
 * no decoding step that can move it: TOML is UTF-8 by definition (v1.0.0
 * "TOML documents must be valid UTF-8 encoded Unicode documents"), so this
 * module refuses any other encoding rather than transcoding it, and the offset
 * is always an index into what the caller passed.
 *
 * `line` and `col` are 1-based. `col` counts characters, not bytes, so a
 * column number points at the right character in a line containing multi-byte
 * text.
 *
 * `context_snippet` is heap-allocated when present and released by
 * gtext_toml_error_free().
 */
typedef struct {
  GTEXT_TOML_Status code; ///< What went wrong.
  const char * message;   ///< Static string; never freed.
  size_t offset;          ///< Byte offset into the input, 0-based.
  int line;               ///< Line, 1-based.
  int col;                ///< Column in characters, 1-based.
  char * context_snippet; ///< Nearby input, or NULL. Freed by
                          ///< gtext_toml_error_free().
  size_t context_snippet_len; ///< Bytes of context_snippet.
  size_t caret_offset;        ///< Byte offset within context_snippet of the
                              ///< position `offset` names.
} GTEXT_TOML_Error;

/**
 * @brief Release anything heap-allocated inside an error.
 *
 * Safe on a zeroed struct, on one a failed parse filled in, and twice.
 *
 * **The snippet comes from the default allocator even when the parse used a
 * caller's**, and this releases it through the default one too. The two have
 * to agree, and an error struct is the one thing that outlives the parse: a
 * caller may free it after releasing the arena its allocator drew from, and a
 * parse can fail before it has read its options at all. So the pair is fixed
 * to the default allocator rather than following the option, and a caller
 * counting every allocation through their own allocator should expect the
 * snippet not to be among them.
 *
 * @param err The error to release. NULL is ignored.
 */
GTEXT_API void gtext_toml_error_free(GTEXT_TOML_Error * err);

/**
 * @enum GTEXT_TOML_Version
 * @brief Which revision of the specification a parse is reading.
 *
 * Three constructs separate the two, and each has cases on both sides of the
 * switch in toml-test's two manifests, so the option is measured at its points
 * of use rather than asserted to exist:
 *
 *   - `\\e` (U+001B) and `\\xHH` escapes in a basic string;
 *   - a newline, a comment, or a trailing comma inside `{ }`;
 *   - a time whose seconds are omitted, which 1.1.0 reads as `:00`.
 *
 * The switch is deliberately not spelled as "relax some checks". 1.1.0 also
 * settles two questions 1.0.0 left contradictory - a lone carriage return
 * inside a multi-line string, which 1.0.0's prose permits and its ABNF
 * forbids - and this module takes the ABNF's reading under both versions, so
 * that a document refused at 1.1.0 is refused at 1.0.0 too. See @ref
 * format_toml for the case-by-case account of what moves and what does not.
 *
 * The writer has no such option, and that is a finding rather than an
 * omission: every spelling 1.0.0 defines is still a 1.1.0 spelling, so a
 * writer emitting 1.0.0 is already correct for both. A `version` field on the
 * write options would be a second spelling of an axis with no point of use.
 */
typedef enum {
  /** TOML v1.0.0, 2021-01-12. The default, and what the oracle implements. */
  GTEXT_TOML_VERSION_1_0_0 = 0,
  /**
   * The TOML v1.1.0 preview.
   *
   * Not a released specification: it is a draft, `tomllib` cannot read it, and
   * the only reference for it is toml-test's own 1.1.0 manifest. A caller who
   * asks for it is asking for a moving target, which is why it is not the
   * default.
   */
  GTEXT_TOML_VERSION_1_1_0 = 1
} GTEXT_TOML_Version;

/**
 * @struct GTEXT_TOML_Parse_Options
 * @brief What a parse is allowed to do.
 *
 * Zeroed is not the default: use gtext_toml_parse_options_default() and change
 * what you mean to change. A zeroed struct asks for no depth limit, which is
 * the one setting this module cannot make safe by construction.
 */
typedef struct {
  /**
   * The allocator every allocation of the parse goes through, or NULL for
   * gtext_allocator_default(). `make check-allocators` keeps this promise
   * true for the files listed there.
   */
  const GTEXT_Allocator * allocator;

  /**
   * Maximum nesting of arrays and inline tables, or 0 for no limit.
   *
   * Default 256. The parser builds values on an explicit stack rather than
   * the C stack, so a limit of 0 does not risk a crash the way it does in a
   * recursive parser - it risks only memory. The limit is here because
   * refusing absurd input early is cheaper than allocating for it.
   */
  size_t max_depth;

  /**
   * Maximum bytes of input, or 0 for no limit. Applied by the file reader
   * while reading, so an over-large file is refused without being held.
   */
  size_t max_total_bytes;

  /**
   * Which revision of the specification to read. Default
   * GTEXT_TOML_VERSION_1_0_0.
   *
   * Zero is the released version, so a zeroed struct asks for 1.0.0 rather
   * than for a draft - the one field here whose zero value is the answer a
   * caller who did not think about it should get.
   */
  GTEXT_TOML_Version version;

  /**
   * Whether comments are kept. Default false.
   *
   * One option with two points of use, deliberately not two options: it
   * decides whether gtext_toml_read_events() reports
   * ::GTEXT_TOML_EVT_COMMENT, and the tree's comments are attached from those
   * same events, so a caller cannot ask for one and get the other. What each
   * of the two keeps is not the same, and it is the format that decides that
   * rather than this option - see gtext_toml_value_leading_comment(), which
   * names the comments a tree can hold and the ones only the event stream can
   * carry.
   *
   * False by default because a comment costs an allocation per statement that
   * has one, and the caller who wants values does not want to pay for text
   * they will not read. Nothing about the *document* changes with it: a
   * comment is not part of TOML's data model, so this cannot turn a document
   * that parses into one that does not.
   */
  bool retain_comments;
} GTEXT_TOML_Parse_Options;

/**
 * @brief The defaults: allocator NULL, max_depth 256, no byte limit, 1.0.0,
 *   comments discarded.
 *
 * @return The defaults, by value.
 */
GTEXT_API GTEXT_TOML_Parse_Options gtext_toml_parse_options_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_TOML_TOML_CORE_H
