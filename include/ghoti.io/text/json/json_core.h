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
 * Core JSON types and definitions.
 *
 * This header provides the core types, enums, and option structures for the
 * JSON module. It does not include the full API headers. Use this for internal
 * implementations that only need type definitions.
 *
 * For the full JSON API, include <ghoti.io/text/json.h> instead.
 */

#ifndef GHOTI_IO_GTEXT_JSON_JSON_CORE_H
#define GHOTI_IO_GTEXT_JSON_JSON_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief JSON operation status codes
 */
typedef enum {
  GTEXT_JSON_OK = 0,

  // General errors
  GTEXT_JSON_E_INVALID,    ///< Invalid input or operation
  GTEXT_JSON_E_OOM,        ///< Out of memory
  GTEXT_JSON_E_LIMIT,      ///< Resource limit exceeded
  GTEXT_JSON_E_DEPTH,      ///< Maximum nesting depth exceeded
  GTEXT_JSON_E_INCOMPLETE, ///< Incomplete input

  // Lexing / parsing specific errors
  GTEXT_JSON_E_BAD_TOKEN,        ///< Invalid token encountered
  GTEXT_JSON_E_BAD_NUMBER,       ///< Invalid number format
  GTEXT_JSON_E_BAD_ESCAPE,       ///< Invalid escape sequence
  GTEXT_JSON_E_BAD_UNICODE,      ///< Invalid Unicode sequence
  GTEXT_JSON_E_TRAILING_GARBAGE, ///< Trailing garbage after valid JSON

  // Semantics / policy errors
  GTEXT_JSON_E_DUPKEY,    ///< Duplicate key in object (when policy is ERROR)
  GTEXT_JSON_E_NONFINITE, ///< Non-finite number when not allowed
  GTEXT_JSON_E_SCHEMA,    ///< Schema validation error

  // Writer errors
  GTEXT_JSON_E_WRITE, ///< Write operation failed
  GTEXT_JSON_E_STATE, ///< Invalid state for operation

  /// Schema uses a standard keyword this implementation does not enforce.
  /// Appended rather than grouped with GTEXT_JSON_E_SCHEMA so that the
  /// numeric value of every pre-existing constant is unchanged.
  GTEXT_JSON_E_SCHEMA_UNSUPPORTED,

  /// JSONPath query is not well-formed (RFC 9535).
  GTEXT_JSON_E_PATH,

  /// JSONPath query is well-formed and uses a construct this implementation
  /// does not evaluate. Separate from GTEXT_JSON_E_PATH because the two ask a
  /// caller for different things: one is a typo, the other is a feature.
  GTEXT_JSON_E_PATH_UNSUPPORTED
} GTEXT_JSON_Status;

/**
 * @brief JSON error information
 *
 * Contains detailed error information including code, message, position,
 * and optional enhanced diagnostics (context snippet, caret positioning,
 * expected/actual token descriptions).
 */
typedef struct {
  GTEXT_JSON_Status code; ///< Error code
  const char * message;   ///< Human-readable error message (static string)
  size_t offset;          ///< Byte offset from start of input (0-based)
  int line;               ///< Line number (1-based)
  int col;                ///< Column number (1-based, byte-based for v1)

  // Enhanced error reporting (optional, may be NULL)
  char * context_snippet;     ///< Context snippet around error (dynamically
                              ///< allocated, caller must free)
  size_t context_snippet_len; ///< Length of context snippet
  size_t
      caret_offset; ///< Byte offset of caret within context snippet (0-based)
  const char * expected_token; ///< Description of expected token (static
                               ///< string, may be NULL)
  const char * actual_token;   ///< Description of actual token encountered
                               ///< (static string, may be NULL)
} GTEXT_JSON_Error;

/**
 * @brief JSON value type enumeration
 */
typedef enum {
  GTEXT_JSON_NULL,   ///< null value
  GTEXT_JSON_BOOL,   ///< boolean value (true/false)
  GTEXT_JSON_NUMBER, ///< number value
  GTEXT_JSON_STRING, ///< string value
  GTEXT_JSON_ARRAY,  ///< array value
  GTEXT_JSON_OBJECT  ///< object value
} GTEXT_JSON_Type;

/**
 * @brief Forward declaration of JSON value structure
 *
 * The actual structure is defined internally. Values are allocated from
 * an arena and freed via gtext_json_free().
 */
typedef struct GTEXT_JSON_Value GTEXT_JSON_Value;

/**
 * @brief Duplicate key handling mode
 */
typedef enum {
  GTEXT_JSON_DUPKEY_ERROR,      ///< Fail parse on duplicate key
  GTEXT_JSON_DUPKEY_FIRST_WINS, ///< Use first occurrence of duplicate key
  GTEXT_JSON_DUPKEY_LAST_WINS,  ///< Use last occurrence of duplicate key
  GTEXT_JSON_DUPKEY_COLLECT     ///< Store duplicates as array (key -> array of
                                ///< values)
} GTEXT_JSON_Dupkey_Mode;

/**
 * @brief Parse options structure
 *
 * Controls parsing behavior including strictness, extensions, limits, and
 * number representation options.
 */
typedef struct {
  // Strictness / extensions
  bool allow_comments;           ///< Allow JSONC comments (// and /* */)
  bool allow_trailing_commas;    ///< Allow trailing commas in arrays/objects
  bool allow_nonfinite_numbers;  ///< Allow NaN, Infinity, -Infinity
  bool allow_single_quotes;      ///< Allow single-quoted strings (relaxed mode)
  bool allow_unescaped_controls; ///< Allow unescaped control characters
                                 ///< (relaxed mode)

  /**
   * Allow a hexadecimal integer: `0x1F`, `0XdeadBEEF`. JSON5. Default: off.
   *
   * A hex literal has no fraction and no exponent, because `e` is one of its
   * digits. Sign applies as it does to a decimal number, so `-0x10` is -16.
   * The preserved lexeme keeps the spelling that was written; `int64` and
   * `uint64` carry the value, and `double` is derived from it, so a literal
   * above 2^53 loses precision in `double` exactly as a long decimal one
   * does.
   */
  bool allow_hex_numbers;

  /**
   * Allow a leading `+` on a number: `+1`, `+1.5`, `+Infinity`. JSON5.
   * Default: off.
   *
   * `+Infinity` and `+NaN` also need allow_nonfinite_numbers, which is what
   * admits the word; this option is only about the sign in front of it.
   * `-NaN` needs only allow_nonfinite_numbers, for the same reason
   * `-Infinity` always has.
   */
  bool allow_leading_plus;

  /**
   * Allow a number whose decimal point has digits on only one side: `.5`,
   * `5.`. JSON5. Default: off.
   *
   * Both spellings, because they are one question: whether the point may sit
   * at an edge. `.` alone is not a number under either setting, and neither
   * is `.e1`.
   */
  bool allow_bare_decimal_point;

  /**
   * Allow ECMAScript's string escapes, which is the set JSON5 uses. JSON5.
   * Default: off.
   *
   * Three things JSON has no spelling for, and one rule that turns an error
   * into a character:
   *
   * - `\xHH` - a codepoint written with two hex digits, so `\xe9` is U+00E9
   *   and comes out as its two UTF-8 bytes rather than as the byte 0xE9.
   * - `\v` - U+000B, the one C escape JSON left out.
   * - `\0` - U+0000, and an error when a digit follows it, because `\01`
   *   would be an octal escape in a language that no longer has them. `\1`
   *   through `\9` are errors for the same reason.
   * - Any other character after a backslash is that character: `\a` is `a`
   *   and `\'` is an apostrophe. A multi-byte character escapes as itself in
   *   full.
   *
   * Line terminators are not in that last rule - a backslash before one is a
   * line continuation, which is allow_line_continuations' question.
   */
  bool allow_ecma_escapes;

  /**
   * Allow a backslash at the end of a line inside a string to continue it on
   * the next line, contributing nothing. JSON5. Default: off.
   *
   * All five line terminator sequences ECMAScript names: LF, CR, CRLF, U+2028
   * and U+2029. CRLF counts as one, so the LF is not left behind to be read as
   * an unescaped control character.
   *
   * Separate from allow_ecma_escapes because it answers a different question -
   * whether a string may span lines at all - and because a caller who wants
   * multi-line strings does not necessarily want `\a` to mean `a`.
   */
  bool allow_line_continuations;

  /**
   * Treat ECMAScript's whitespace as whitespace between tokens. JSON5.
   * Default: off.
   *
   * JSON allows four characters between tokens: tab, LF, CR and space.
   * ECMAScript - and so JSON5 - also allows vertical tab, form feed, the
   * zero-width no-break space U+FEFF, every character in General_Category Zs
   * (which includes the no-break space U+00A0 and the ideographic space
   * U+3000), and the line terminators U+2028 and U+2029.
   *
   * Zs is asked of ghoti.io-unicode rather than written out here, because
   * that category has changed: U+180E was Zs until Unicode 6.3 moved it to
   * Cf. `make check-ucd-pin` holds that library's UCD version and this one's
   * IDNA pin to the same value.
   *
   * This is about the space *between* tokens. It says nothing about what may
   * appear inside a string, where JSON already allows every one of these
   * except a raw CR or LF.
   */
  bool allow_ecma_whitespace;

  /**
   * Allow an unquoted object name: `{a: 1}`. JSON5. Default: off.
   *
   * The name is an ECMAScript `IdentifierName`, which is more than
   * `[A-Za-z_]`: any character with the Unicode property ID_Start may begin
   * one and any with ID_Continue may continue it, `$` and `_` may do either,
   * and `\uXXXX` escapes are allowed - `{\u0061: 1}` names `a`. The
   * properties are ghoti.io-unicode's, which is where the suite keeps one
   * copy of the UCD.
   *
   * `IdentifierName` includes the reserved words, so `{true: 1}` is an object
   * whose name is the three letters `true`, and `{null: 1}`, `{NaN: 1}` and
   * `{Infinity: 1}` likewise. Those spellings are still keywords everywhere a
   * *value* is expected.
   *
   * An escape is decoded before the name is compared, so `{"a":1,\u0061:2}`
   * is one name written twice and the duplicate-name policy sees it as such.
   * With normalize_unicode, a name arrives normalized whether it was quoted or
   * not.
   *
   * `\u{1F600}`, ECMAScript 2015's other escape spelling, is not accepted: the
   * JSON5 specification is written against ECMAScript 5.1, which has only the
   * four-digit form. A surrogate pair of four-digit escapes does reach an
   * astral character, as it does there.
   */
  bool allow_unquoted_keys;

  // Unicode / input handling
  bool allow_leading_bom; ///< Allow leading UTF-8 BOM (default: on)
  bool validate_utf8;     ///< Validate UTF-8 sequences (default: on)
  /**
   * Normalize every string to Unicode NFC. Default: off.
   *
   * Applies to object names as well as values, which is what makes
   * duplicate-name detection meaningful: `{"\u00e9":1,"e\u0301":2}` is one
   * name written twice, and with this off it is two. Normalization happens
   * before the parser compares names, so dupkeys sees the normalized form.
   *
   * Two interactions, both deliberate:
   *
   * - **Requires validate_utf8**, which is on by default. Normalizing bytes
   *   that have not been established as text is not defined; asking for one
   *   without the other fails the parse with GTEXT_JSON_E_INVALID rather than
   *   normalizing some strings and not others.
   * - **Disables in_situ_mode for strings.** Normalization has to copy, and
   *   the length equality in-situ tests for is not evidence the bytes are
   *   unchanged - canonical ordering reorders combining marks without changing
   *   how many bytes they occupy. Numbers are still referenced in place.
   *
   * A string that is not well-formed UTF-8 fails with
   * GTEXT_JSON_E_BAD_UNICODE, the same status the validator uses.
   */
  bool normalize_unicode;
  bool in_situ_mode;      ///< Zero-copy mode: reference input buffer directly

  /**
   * Allocator for everything the parse produces, or NULL for
   * gtext_allocator_default().
   *
   * The DOM records it, so gtext_json_free() releases through the same
   * allocator without the caller passing it again. It must stay valid for
   * the lifetime of the value the parse returns.
   *
   * Covered: gtext_json_parse(), gtext_json_parse_multiple() and
   * gtext_json_parse_file() - the arena and every DOM node, key and string in
   * it, the preserved number lexemes, and the parser's transient buffers.
   *
   * Also covered, because both take these same options:
   * gtext_json_stream_new() - the stream structure, its input buffer, the
   * token buffer, the state stack and each object's key copies - and
   * gtext_json_reader_new(), whose event queue was already on this allocator
   * and whose stream now is too.
   *
   * And covered by inheritance, without taking these options at all: whatever
   * the parse recorded here is read back from the tree by
   * gtext_json_patch_apply(), gtext_json_merge_patch() and
   * gtext_json_clone(), each of which clones a document and used to put that
   * copy in the C library.
   *
   * Not covered, and still using the C library:
   * - `GTEXT_JSON_Error::context_snippet`, because gtext_json_error_free()
   *   receives only the error and has no way to learn which allocator made
   *   it. Freeing it through a mismatched allocator would be worse than the
   *   one diagnostic allocation it avoids.
   * - JSON Schema, which does not take an allocator yet.
   *
   * The writer and JSON Pointer are covered and are not reached through this
   * member, because neither takes parse options: see
   * GTEXT_JSON_Write_Options::allocator below, and
   * gtext_json_pointer_get_with_allocator().
   *
   * Nothing silently falls back: `make check-allocators` fails the build if a
   * file on the covered list calls malloc, calloc, realloc or free directly.
   */
  const GTEXT_Allocator * allocator;
                          ///< (default: off)

  // Duplicate keys
  GTEXT_JSON_Dupkey_Mode dupkeys; ///< Duplicate key handling policy

  // Limits (0 => library default)
  /**
   * Maximum nesting depth (0 = default, 256).
   *
   * This is not only a resource limit, because gtext_json_parse() is recursive
   * descent: a document's nesting depth is the parser's stack depth, measured
   * at about **448 bytes per level** (calibrated by bisecting the depth at
   * which the parse dies, at four stack sizes - 445, 447, 448 and 447 bytes
   * per level at 1, 2, 4 and 8 MiB). The default 256 needs some 115 KB and is
   * safe on any stack a thread is likely to have; raising this to N is a claim
   * that the calling thread can hold N times that.
   *
   * As a rule of thumb, the ceiling is about 2,300 levels on a 1 MiB thread
   * stack and 18,000 on Linux's 8 MiB main stack. Past it the parse does not
   * return an error - the process dies - so a caller raising the limit for
   * untrusted input is choosing a number rather than removing one.
   *
   * gtext_json_stream_feed() has no such bound: the streaming parser keeps its
   * nesting stack on the heap, so max_depth there is a resource limit and
   * nothing else, and a 50,000-level document goes through it. For deep or
   * untrusted input, that is the parser to use.
   */
  size_t max_depth;
  size_t max_string_bytes; ///< Maximum string size in bytes (0 = default, e.g.
                           ///< 16MB)
  size_t max_container_elems; ///< Maximum array/object elements (0 = default,
                              ///< e.g. 1M)
  size_t max_total_bytes; ///< Maximum total input size (0 = default, e.g. 64MB)

  // Number fidelity / representations
  bool
      preserve_number_lexeme; ///< Preserve original number token for round-trip
  bool parse_int64;           ///< Detect and parse exact int64 representation
  bool parse_uint64;          ///< Detect and parse exact uint64 representation
  bool parse_double; ///< Derive double representation when representable
} GTEXT_JSON_Parse_Options;

/**
 * @brief Floating-point formatting strategy
 */
typedef enum {
  GTEXT_JSON_FLOAT_SHORTEST, ///< Shortest representation (%.17g, default)
  GTEXT_JSON_FLOAT_FIXED, ///< Fixed-point notation (%.Nf, use float_precision)
  GTEXT_JSON_FLOAT_SCIENTIFIC ///< Scientific notation (%.Ne, use
                              ///< float_precision)
} GTEXT_JSON_Float_Format;

/**
 * @brief Write options structure
 *
 * Controls serialization behavior including formatting, escaping, and
 * canonical output options.
 */
typedef struct {
  /**
   * The allocator the writer's own working memory comes from, or NULL for the
   * default. **Not the sink's**: a buffer sink owns its buffer, because a sink
   * is created before any options are seen and outlives the write.
   *
   * What this covers is the scratch a write needs and then releases - the
   * frame stack the value walk carries, and the sorted index array a
   * ::sort_object_keys write builds per object - plus, for the incremental
   * API, the ::GTEXT_JSON_Writer handle and its stack, which
   * gtext_json_writer_new() allocates and gtext_json_writer_free() releases
   * through this same allocator. So an arena caller writing a large document
   * no longer reaches the C heap for any of it.
   *
   * The same line GTEXT_INI_Write_Options::allocator draws, and for the same
   * reason: a partial allocator is worse than none, so the sink's buffer is
   * exempt deliberately and documented rather than left to be discovered.
   */
  const GTEXT_Allocator * allocator;

  // Formatting
  bool pretty;       ///< Pretty-print output (false = compact, true = pretty)
  int indent_spaces; ///< Number of spaces per indent level (e.g. 2, 4)
  const char * newline;   ///< Newline string ("\n" default, allow "\r\n")
  bool trailing_newline;  ///< Add trailing newline at end of output (default:
                          ///< false)
  bool space_after_colon; ///< Add space after ':' in objects (default: false)
  bool space_after_comma; ///< Add space after ',' in arrays/objects (default:
                          ///< false)
  int inline_array_threshold;  ///< Max elements for inline array (0=always
                               ///< pretty, -1=always inline, default: -1)
  int inline_object_threshold; ///< Max pairs for inline object (0=always
                               ///< pretty, -1=always inline, default: -1)

  // Escaping
  bool escape_solidus;       ///< Escape forward slash (optional)
  bool escape_unicode;       ///< Output \\uXXXX for non-ASCII (canonical mode)
  bool escape_all_non_ascii; ///< Escape all non-ASCII characters (stricter)

  // Canonical / deterministic
  bool sort_object_keys;  ///< Sort object keys for stable output
  bool canonical_numbers; ///< Normalize numeric lexemes (use with care)

  // Extensions
  bool allow_nonfinite_numbers; ///< Emit NaN/Infinity if node contains it

  // Floating-point formatting
  GTEXT_JSON_Float_Format
      float_format; ///< Floating-point formatting strategy (default: SHORTEST)
  int float_precision; ///< Precision for fixed/scientific format (default: 6,
                       ///< ignored for SHORTEST)
} GTEXT_JSON_Write_Options;

/**
 * @brief Initialize parse options with strict JSON defaults
 *
 * Returns a parse options structure with:
 * - Strict JSON mode (all extensions off)
 * - UTF-8 validation enabled
 * - Number lexeme preservation enabled
 * - Duplicate key policy: ERROR
 * - All limits set to 0 (library defaults)
 *
 * @return Initialized parse options structure
 */
GTEXT_API GTEXT_JSON_Parse_Options gtext_json_parse_options_default(void);

/**
 * @brief Initialize parse options for the JSON5 dialect
 *
 * The defaults above, with every option JSON5 requires turned on:
 * comments, trailing commas, single-quoted strings, non-finite numbers,
 * hexadecimal integers, a leading plus, a decimal point at an edge,
 * ECMAScript's string escapes, line continuations, ECMAScript's whitespace,
 * and unquoted object names.
 *
 * The individual options remain the interface. This is the dialect as
 * [json5.org](https://json5.org/) version 1.0.0 defines it, for a caller who
 * wants all of it rather than a chosen subset; it is a starting point like the
 * default, so a caller may still change any field afterwards - the limits and
 * the duplicate-name policy are left exactly as the default has them.
 *
 * What it does *not* turn on is anything JSON5 does not ask for.
 * `allow_unescaped_controls` stays off, because JSON5 no more permits a raw
 * control character in a string than JSON does, and `normalize_unicode` stays
 * off because JSON5 says nothing about normalization.
 *
 * @return Initialized parse options structure
 */
GTEXT_API GTEXT_JSON_Parse_Options gtext_json_parse_options_json5(void);

/**
 * @brief Initialize write options with compact output defaults
 *
 * Returns a write options structure with:
 * - Compact output (pretty = 0)
 * - Standard escaping
 * - No canonical formatting
 *
 * @return Initialized write options structure
 */
GTEXT_API GTEXT_JSON_Write_Options gtext_json_write_options_default(void);

/**
 * @brief Free a JSON value and its entire DOM tree
 *
 * Frees the arena associated with the value, which deallocates
 * all nodes and strings in the DOM tree. After calling this function,
 * the value pointer and all pointers to values in the tree are invalid.
 *
 * @param v Value to free (can be NULL, in which case this is a no-op)
 */
GTEXT_API void gtext_json_free(GTEXT_JSON_Value * v);

/**
 * @brief Free the context snippet in an error structure
 *
 * Frees the dynamically allocated context snippet in a GTEXT_JSON_Error
 * structure. This should be called when the error structure is no longer
 * needed to prevent memory leaks. Other fields (message, expected_token,
 * actual_token) are static strings and should not be freed.
 *
 * @param err Error structure to clean up (can be NULL, in which case this is a
 * no-op)
 */
GTEXT_API void gtext_json_error_free(GTEXT_JSON_Error * err);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_JSON_JSON_CORE_H
