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
 * Internal definitions for JSON module implementation.
 *
 * This header contains internal-only definitions used by the JSON module
 * implementation. It should not be included by external code.
 */

#ifndef GHOTI_IO_GTEXT_SRC_JSON_JSON_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_JSON_JSON_INTERNAL_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_schema.h>
#include <ghoti.io/text/json/json_writer.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default limits for JSON parsing (used when opts->max_* is 0)
 */
#define JSON_DEFAULT_MAX_DEPTH 256
#define JSON_DEFAULT_MAX_STRING_BYTES (16 * 1024 * 1024) // 16MB
#define JSON_DEFAULT_MAX_CONTAINER_ELEMS (1024 * 1024)   // 1M
#define JSON_DEFAULT_MAX_TOTAL_BYTES (64 * 1024 * 1024)  // 64MB

/**
 * @brief Position tracking structure for string processing
 */
typedef struct {
  size_t offset; ///< Byte offset from start
  int line;      ///< Line number (1-based)
  int col;       ///< Column number (1-based, byte-based)
} json_position;

/**
 * @brief UTF-8 handling mode
 */
typedef enum {
  JSON_UTF8_REJECT,  ///< Reject invalid UTF-8 sequences
  JSON_UTF8_REPLACE, ///< Replace invalid sequences with replacement character
  JSON_UTF8_VERBATIM ///< Allow invalid sequences verbatim
} json_utf8_mode;

/**
 * @brief Check if a length-delimited string exactly equals a null-terminated
 * keyword
 *
 * This utility function compares a length-delimited string (input with length
 * len) against a null-terminated keyword string. Useful for matching JSON
 * keywords like "true", "false", "null", "NaN", "Infinity", etc.
 *
 * @param input Input string (may not be null-terminated)
 * @param len Length of input string
 * @param keyword Null-terminated keyword to match against
 * @return 1 if strings match exactly (case-sensitive), 0 otherwise
 */
int json_matches(const char * input, size_t len, const char * keyword);

/**
 * @brief Decode a JSON string with escape sequences
 *
 * This function decodes a JSON string, handling:
 * - Standard escape sequences (\", \\, \/, \b, \f, \n, \r, \t)
 * - Unicode escapes (\uXXXX)
 * - Surrogate pairs (\uD83D\uDE00)
 *
 * The function performs bounds checking to prevent buffer overflow.
 * If the decoded string would exceed the output buffer capacity,
 * GTEXT_JSON_E_LIMIT is returned.
 *
 * @param input Input string (without surrounding quotes)
 * @param input_len Length of input
 * @param output Output buffer
 * @param output_capacity Capacity of output buffer in bytes
 * @param output_len Output: length of decoded string
 * @param pos Input/output: position tracking (can be NULL)
 * @param validate_utf8 Whether to validate UTF-8
 * @param utf8_mode UTF-8 handling mode if validation fails
 * @param allow_unescaped_controls Whether to allow unescaped control characters
 * (0x00-0x1F)
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_INTERNAL_API GTEXT_JSON_Status json_decode_string(const char * input,
    size_t input_len, char * output, size_t output_capacity,
    size_t * output_len, json_position * pos, json_utf8_mode utf8_mode,
    const GTEXT_JSON_Parse_Options * opts);

/**
 * @brief Number representation flags
 */
typedef enum {
  JSON_NUMBER_HAS_LEXEME = 1,   ///< Lexeme is preserved
  JSON_NUMBER_HAS_I64 = 2,      ///< int64 representation is valid
  JSON_NUMBER_HAS_U64 = 4,      ///< uint64 representation is valid
  JSON_NUMBER_HAS_DOUBLE = 8,   ///< double representation is valid
  JSON_NUMBER_IS_NONFINITE = 16 ///< Number is NaN, Infinity, or -Infinity
} json_number_flags;

/**
 * @brief Parsed number structure
 *
 * Holds all representations of a parsed number along with flags
 * indicating which representations are valid.
 *
 * This is a temporary parsing structure used internally. When the lexeme
 * is preserved (via preserve_number_lexeme option), memory is allocated
 * with malloc() and must be freed using json_number_destroy().
 *
 * Note: This structure is separate from GTEXT_JSON_Value.as.number which
 * uses arena allocation and is automatically cleaned up via gtext_json_free().
 * When converting from json_number to GTEXT_JSON_Value, the data should
 * be copied into the arena and json_number_destroy() should be called
 * to free the temporary structure.
 */
typedef struct {
  /// Allocator that owns `lexeme`, or NULL for the default.  Carried in the
  /// struct rather than passed to json_number_destroy(), which has 97 call
  /// sites; this way the free always matches the allocation with no
  /// signature churn.
  const GTEXT_Allocator * alloc;
  char * lexeme;      ///< Original number lexeme (owned by `alloc`)
  size_t lexeme_len;  ///< Length of lexeme
  int64_t i64;        ///< int64 representation
  uint64_t u64;       ///< uint64 representation
  double dbl;         ///< double representation
  unsigned int flags; ///< Flags indicating valid representations
} json_number;

/**
 * @brief A URI split into its RFC 3986 section 3 components
 *
 * Every pointer is into the string that was split, so none of it outlives
 * that string. The `has_*` flags distinguish a component that was present and
 * empty from one that was absent, which section 5 treats differently.
 */
typedef struct {
  const char * scheme;
  size_t scheme_len;
  const char * authority;
  size_t authority_len;
  const char * path;
  size_t path_len;
  const char * query;
  size_t query_len;
  const char * fragment;
  size_t fragment_len;
  int has_scheme;
  int has_authority;
  int has_query;
  int has_fragment;
} json_uri_parts;

/**
 * @brief Split a URI into its components
 */
GTEXT_INTERNAL_API void json_uri_parts_split(
    const char * uri, size_t len, json_uri_parts * out);

/**
 * @brief Resolve a URI-reference against a base, RFC 3986 section 5.2
 *
 * @param alloc Allocator the result comes from, or NULL for the default
 * @return A string the caller releases through @p alloc, or NULL if
 *   allocation failed
 */
GTEXT_INTERNAL_API char * json_uri_resolve(
    const char * base, size_t base_len, const char * ref, size_t ref_len,
    const GTEXT_Allocator * alloc);

/**
 * @brief Copy a URI with its fragment removed
 *
 * @param alloc Allocator the result comes from, or NULL for the default
 * @return A string the caller releases through @p alloc, or NULL if
 *   allocation failed
 */
GTEXT_INTERNAL_API char * json_uri_without_fragment(
    const char * uri, size_t len, const GTEXT_Allocator * alloc);

/**
 * @brief Percent-decode a string, RFC 3986 section 2.1
 *
 * @param alloc Allocator the result comes from, or NULL for the default
 * @return A string the caller releases through @p alloc, or NULL if
 *   allocation failed
 */
GTEXT_INTERNAL_API char * json_uri_percent_decode(
    const char * s, size_t len, size_t * out_len,
    const GTEXT_Allocator * alloc);

/**
 * @brief Is `name` one of the formats 2020-12's vocabulary defines?
 *
 * A name in the vocabulary is one a caller asking for assertion can expect to
 * be checked; a name outside it belongs to nobody and is ignored.
 */
GTEXT_INTERNAL_API int json_format_is_known(const char * name, size_t len);

/**
 * @brief Does `value` satisfy the named format?
 *
 * @return 1 if it does or the format is not one this checks, 0 if it does not
 */
GTEXT_INTERNAL_API int json_format_check(
    const char * name, size_t name_len, const char * value, size_t value_len);

/**
 * @brief A JSON number as an exact decimal: mantissa * 10^exponent
 *
 * JSON numbers are decimal text. Keywords that ask an arithmetic question
 * about one - `multipleOf` is the only one - have to ask it in decimal, or
 * they answer a different question than the document wrote.
 */
typedef struct {
  uint64_t mantissa; ///< Significant digits, trailing zeros moved to exponent
  int64_t exponent;  ///< Power of ten the mantissa is scaled by
  int negative;      ///< 1 if the lexeme carried a minus sign
} json_decimal;

/**
 * @brief Read a number lexeme as an exact decimal
 *
 * @param lexeme The number token, as the document spelled it
 * @param len Its length
 * @param out Output decimal
 * @return 1 if it was read exactly, 0 if it does not fit (more than 19
 *   significant digits, an absurd exponent, or not a bare number lexeme)
 */
GTEXT_INTERNAL_API int json_decimal_from_lexeme(
    const char * lexeme, size_t len, json_decimal * out);

/**
 * @brief Does a number lexeme denote a whole number?
 *
 * Asked of the digits rather than of the double they became, because the two
 * disagree in both directions at the edges: a fifty-three digit integer is not
 * a representable double and is still an integer, and the nearest double to
 * `1.00000000000000000000000000000000001e30` is whole while the value is not.
 * No arithmetic is done on the digits, so there is no magnitude at which this
 * stops working.
 *
 * @return 1 whole, 0 not, -1 undecidable (not a bare JSON number lexeme)
 */
GTEXT_INTERNAL_API int json_decimal_lexeme_is_integer(
    const char * lexeme, size_t len);

/**
 * @brief Is `value` an exact integer multiple of `divisor`?
 *
 * @return 1 yes, 0 no, -1 undecidable (a NULL argument or a zero divisor)
 */
GTEXT_INTERNAL_API int json_decimal_is_multiple_of(
    const json_decimal * value, const json_decimal * divisor);

/**
 * @brief Parse a JSON number token
 *
 * This function parses a JSON number token, performing:
 * - Syntax validation (reject invalid forms like 01, 1., .1, etc.)
 * - Lexeme preservation (store exact token text)
 * - int64 detection and parsing with overflow detection
 * - uint64 detection and parsing
 * - double derivation using strtod
 * - Nonfinite number support (NaN, Infinity, -Infinity) when enabled
 *
 * The function validates the number format according to RFC 8259.
 * Invalid formats are rejected with GTEXT_JSON_E_BAD_NUMBER.
 *
 * @param input Input string containing the number
 * @param input_len Length of input
 * @param num Output: parsed number structure
 * @param pos Input/output: position tracking (can be NULL)
 * @param opts Parse options (for nonfinite numbers, etc.)
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_INTERNAL_API GTEXT_JSON_Status json_parse_number(const char * input,
    size_t input_len, json_number * num, json_position * pos,
    const GTEXT_JSON_Parse_Options * opts);

/**
 * @brief Free resources allocated by json_parse_number()
 *
 * This function frees the lexeme string allocated by json_parse_number()
 * when preserve_number_lexeme was enabled. It should be called when a
 * json_number structure is no longer needed to prevent memory leaks.
 *
 * **When to call:**
 * - After using json_parse_number() standalone (not converting to
 * GTEXT_JSON_Value)
 * - After converting json_number data to GTEXT_JSON_Value (before discarding
 * json_number)
 * - On error paths where json_parse_number() succeeded but subsequent
 * operations failed
 *
 * **When NOT to call:**
 * - If json_number data has been copied into GTEXT_JSON_Value (which uses arena
 * allocation) In that case, gtext_json_free() on the root value will clean up
 * all memory.
 *
 * After calling this function, the json_number structure should be considered
 * invalid and should not be used unless json_parse_number() is called again.
 *
 * @param num Number structure to clean up (can be NULL, safe to call multiple
 * times)
 */
GTEXT_INTERNAL_API void json_number_destroy(json_number * num);

/**
 * @brief JSON token types
 */
typedef enum {
  JSON_TOKEN_EOF,         ///< End of input
  JSON_TOKEN_ERROR,       ///< Error token
  JSON_TOKEN_LBRACE,      ///< {
  JSON_TOKEN_RBRACE,      ///< }
  JSON_TOKEN_LBRACKET,    ///< [
  JSON_TOKEN_RBRACKET,    ///< ]
  JSON_TOKEN_COLON,       ///< :
  JSON_TOKEN_COMMA,       ///< ,
  JSON_TOKEN_NULL,        ///< null keyword
  JSON_TOKEN_TRUE,        ///< true keyword
  JSON_TOKEN_FALSE,       ///< false keyword
  JSON_TOKEN_STRING,      ///< String value
  JSON_TOKEN_NUMBER,      ///< Number value
  JSON_TOKEN_IDENT,       ///< Unquoted object name (JSON5), value in
                          ///< data.string like a string token
  JSON_TOKEN_NAN,         ///< NaN (extension)
  JSON_TOKEN_INFINITY,    ///< Infinity (extension)
  JSON_TOKEN_NEG_INFINITY ///< -Infinity (extension)
} json_token_type;

/**
 * @brief JSON token structure
 *
 * Represents a single token from the lexer, including its type,
 * position information, and value data.
 */
typedef struct {
  json_token_type type; ///< Token type
  json_position pos;    ///< Position where token starts
  size_t length;        ///< Length of token in input (bytes)

  // Value data (only valid for certain token types)
  union {
    struct {
      /// Allocator that owns `value`, or NULL for the default.  Same
      /// reasoning as json_number::alloc.
      const GTEXT_Allocator * alloc;
      char * value;     ///< Decoded string value (allocated, caller must free)
      size_t value_len; ///< Length of decoded string
      size_t original_start; ///< Original string start position in input (after
                             ///< opening quote, for in-situ mode)
      size_t original_len;   ///< Original string content length in input (for
                             ///< in-situ mode)
    } string;
    json_number number; ///< Parsed number (temporary, use json_number_destroy)
  } data;
} json_token;

// Forward declaration (defined in json_stream_internal.h)
struct json_token_buffer;

/**
 * @brief JSON lexer structure
 *
 * Internal lexer state for tokenizing JSON input.
 */
typedef struct {
  const char * input;    ///< Input buffer
  size_t input_len;      ///< Total input length
  size_t current_offset; ///< Current position in input
  json_position pos;     ///< Current position (offset, line, col)
  const GTEXT_JSON_Parse_Options * opts; ///< Parse options
  int streaming_mode; ///< Non-zero if in streaming mode (allows incomplete
                      ///< tokens at EOF)
  struct json_token_buffer *
      token_buffer; ///< Token buffer for incomplete tokens (streaming mode
                    ///< only, can be NULL)

  /**
   * Set when the white space or comment this lexer skipped ended a line.
   *
   * GTEXT_JSON_RECORDS_LINE has to know whether a record held a line end of
   * its own, and the only code that sees those bytes is
   * json_lexer_skip_whitespace(): the line ends *between* records are consumed
   * by the streaming parser's own separator scan, so anything this flag
   * reports happened inside a value. The lexer never clears it - the streaming
   * parser collects it after each token and clears it there, because a lexer
   * is re-initialised for each buffer and a flag it owned would be lost at
   * every chunk boundary.
   */
  int saw_line_end;
} json_lexer;

/**
 * @brief Initialize a lexer
 *
 * @param lexer Lexer structure to initialize
 * @param input Input buffer (must remain valid for lexer lifetime)
 * @param input_len Length of input buffer
 * @param opts Parse options (can be NULL for defaults)
 * @return GTEXT_JSON_OK on success
 */
GTEXT_INTERNAL_API GTEXT_JSON_Status json_lexer_init(json_lexer * lexer,
    const char * input, size_t input_len, const GTEXT_JSON_Parse_Options * opts,
    int streaming_mode, int at_input_start);

/**
 * @brief Get the next token from the lexer
 *
 * Advances the lexer position and returns the next token.
 * The token's data (string value, number) must be cleaned up
 * by the caller using json_token_cleanup().
 *
 * @param lexer Lexer instance
 * @param token Output token structure
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_JSON_Status json_lexer_next(json_lexer * lexer, json_token * token);

/**
 * @brief Clean up resources allocated by a token
 *
 * Frees any memory allocated for token data (string values,
 * number lexemes). Should be called after processing a token.
 *
 * @param token Token to clean up
 */
GTEXT_INTERNAL_API void json_token_cleanup(json_token * token);

/**
 * @brief Arena block structure
 *
 * Each block contains a chunk of memory that can be allocated from.
 * Blocks are linked together to form the arena.
 *
 * WARNING: This struct must NEVER be passed by value or copied.
 * It uses a flexible array member pattern and must always be used
 * as a pointer. Copying will only copy the struct header, not the
 * allocated data.
 */
typedef struct json_arena_block {
  struct json_arena_block * next; ///< Next block in the arena
  size_t used;                    ///< Bytes used in this block
  size_t size;                    ///< Total size of this block
  char data[1]; ///< Flexible array member for block data (C99 FAM, use data[1]
                ///< for C++ compatibility)
} json_arena_block;

/**
 * @brief Arena allocator structure
 *
 * Manages a collection of blocks for efficient bulk allocation.
 * All memory is freed when the arena is destroyed.
 */
typedef struct json_arena {
  json_arena_block * first;   ///< First block in the arena
  json_arena_block * current; ///< Current block being used
  size_t block_size;          ///< Size of each new block
  const GTEXT_Allocator * alloc; ///< Allocator for blocks (never NULL)
} json_arena;

// JSON context structure
// Holds the arena allocator and other context information
// for a JSON DOM tree.
typedef struct json_context {
  json_arena * arena; ///< Arena allocator for this DOM
  const GTEXT_Allocator * alloc; ///< Allocator backing the arena (never NULL)
  const char *
      input_buffer; ///< Original input buffer (for in-situ mode, caller-owned)
  size_t input_buffer_len; ///< Length of input buffer (for in-situ mode)
} json_context;

// Internal structure definition for GTEXT_JSON_Value
// This is needed by the parser to manipulate arrays and objects
struct GTEXT_JSON_Value {
  GTEXT_JSON_Type type; ///< Type of this value
  json_context * ctx;   ///< Context (arena) for this value tree
  /** Whether this value has ever been stored inside another value.
   *
   * Read only by json_check_no_cycle(), and only as a shortcut: a value that
   * nothing points at cannot be reached from anything, so an insertion whose
   * *container* is not contained cannot close a cycle and needs no walk. That
   * turns assembling a deep value inside-out - wrap the big thing in a fresh
   * container, repeat - from quadratic in its depth back into linear, which at
   * sixteen thousand levels is the difference between 6.7 seconds and 1.5
   * milliseconds.
   *
   * **Every door that stores one value inside another must set it**, or the
   * shortcut skips a walk that was needed. The four public ones funnel through
   * json_array_add_element() and json_object_add_pair() or assign in place
   * beside their own guard, which is why all of it is in one file. The parser,
   * the clone and the patch builders do not set it and do not need to: they
   * build fresh subtrees whose interior nodes a caller can only reach through
   * the const accessors, so none of them can arrive here as a container. */
  bool contained;

  union {
    int boolean; ///< For GTEXT_JSON_BOOL
    struct {
      char * data;    ///< String data (null-terminated, may point into input
                      ///< buffer in in-situ mode)
      size_t len;     ///< String length in bytes
      int is_in_situ; ///< 1 if data points into input buffer (caller-owned), 0
                      ///< if arena-allocated
    } string;         ///< For GTEXT_JSON_STRING
    struct {
      char * lexeme; ///< Original number lexeme (may point into input buffer in
                     ///< in-situ mode)
      size_t lexeme_len; ///< Length of lexeme
      int is_in_situ; ///< 1 if lexeme points into input buffer (caller-owned),
                      ///< 0 if arena-allocated
      int64_t i64;    ///< int64 representation (if valid)
      uint64_t u64;   ///< uint64 representation (if valid)
      double dbl;     ///< double representation (if valid)
      int has_i64;    ///< 1 if i64 is valid
      int has_u64;    ///< 1 if u64 is valid
      int has_dbl;    ///< 1 if dbl is valid
    } number;         ///< For GTEXT_JSON_NUMBER
    struct {
      GTEXT_JSON_Value ** elems; ///< Array of value pointers
      size_t count;              ///< Number of elements
      size_t capacity;           ///< Allocated capacity
    } array;                     ///< For GTEXT_JSON_ARRAY
    struct {
      struct {
        char * key;               ///< Object key
        size_t key_len;           ///< Key length
        GTEXT_JSON_Value * value; ///< Object value
      } * pairs;                  ///< Array of key-value pairs
      size_t count;               ///< Number of pairs
      size_t capacity;            ///< Allocated capacity
    } object;                     ///< For GTEXT_JSON_OBJECT
  } as;
};

/**
 * @brief Create a JSON value using an existing context
 *
 * Internal function for parser use. Creates a value that shares
 * the same context (arena) as other values in the parse tree.
 *
 * @param type Type of value to create
 * @param ctx Existing context to use
 * @return New value, or NULL on failure
 */
GTEXT_JSON_Value * json_value_new_with_existing_context(
    GTEXT_JSON_Type type, json_context * ctx);

/**
 * @brief Allocate memory from a context's arena
 *
 * Internal function for parser use. Allocates memory from the
 * arena associated with a context.
 *
 * @param ctx Context containing the arena
 * @param size Size in bytes to allocate
 * @param align Alignment requirement (must be power of 2)
 * @return Pointer to allocated memory, or NULL on failure
 */
void * json_arena_alloc_for_context(
    json_context * ctx, size_t size, size_t align);

/**
 * @brief Create a new JSON context with arena
 *
 * Internal function for creating a new context with an arena allocator.
 * Used by patch implementation for atomic operations.
 *
 * @return New context, or NULL on failure
 */
json_context * json_context_new(const GTEXT_Allocator * alloc);

/**
 * @brief Set input buffer for in-situ mode
 *
 * Internal function for storing a reference to the input buffer in the context.
 * The buffer is caller-owned and must remain valid for the lifetime of the DOM.
 *
 * @param ctx Context to set input buffer on (must not be NULL)
 * @param input_buffer Original input buffer (caller-owned, must remain valid)
 * @param input_buffer_len Length of input buffer
 */
void json_context_set_input_buffer(
    json_context * ctx, const char * input_buffer, size_t input_buffer_len);

/**
 * @brief Free a JSON context and its arena
 *
 * Internal function for freeing a context and its associated arena.
 * Used by patch implementation for cleanup.
 * Note: The input buffer (if set) is caller-owned and is NOT freed here.
 *
 * @param ctx Context to free (can be NULL)
 */
void json_context_free(json_context * ctx);

/**
 * @brief Add an element to a JSON array
 *
 * Internal function for parser use. Adds an element to an array,
 * growing the array if necessary.
 *
 * @param array Array value (must be GTEXT_JSON_ARRAY type)
 * @param element Element value to add
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_JSON_Status json_array_add_element(
    GTEXT_JSON_Value * array, GTEXT_JSON_Value * element);

/**
 * @brief Add a key-value pair to a JSON object
 *
 * Internal function for parser use. Adds a key-value pair to an object,
 * growing the object if necessary.
 *
 * @param object Object value (must be GTEXT_JSON_OBJECT type)
 * @param key Key string (will be copied into arena)
 * @param key_len Length of key string
 * @param value Value to associate with key
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_JSON_Status json_object_add_pair(GTEXT_JSON_Value * object,
    const char * key, size_t key_len, GTEXT_JSON_Value * value);

/**
 * @brief Deep clone a JSON value into a context
 *
 * Creates a deep copy of a JSON value, allocating all memory from the
 * specified context's arena. Used by patch operations and schema validation
 * for cloning values.
 *
 * @param src Source value to clone (must not be NULL)
 * @param ctx Context with arena for allocation (must not be NULL)
 * @return Cloned value, or NULL on allocation failure
 */
GTEXT_JSON_Value * json_value_clone(
    const GTEXT_JSON_Value * src, json_context * ctx);

/**
 * @brief Deep clone a JSON value into a context of its own
 *
 * The clone owns the context it is in, so gtext_json_free() releases it.
 * Unlike json_value_clone(), which puts the copy in a caller's context and
 * ties its lifetime to that context's, this is for a copy that outlives
 * whatever it was copied from.
 *
 * @param src Source value to clone, or NULL for NULL
 * @param alloc Allocator the new context draws from, or NULL for the default.
 *   Named rather than taken from @p src, because a copy that outlives the
 *   original may need to outlive the original's allocator too; gtext_json_clone()
 *   passes the source's, and the compiled schema passes its own.
 * @return Cloned value, or NULL on allocation failure
 */
GTEXT_INTERNAL_API GTEXT_JSON_Value * json_value_clone_new_context(
    const GTEXT_JSON_Value * src, const GTEXT_Allocator * alloc);

/**
 * @brief Create a string value in a context of its own, on a named allocator
 *
 * What gtext_json_new_string() does, with the allocator spelled out. The
 * public builder takes no options and so passes NULL; a caller inside the
 * library that has an allocator in hand passes that instead, so a value it
 * builds and frees does not bypass what the caller asked for.
 *
 * @param s String bytes, or NULL only when @p len is 0
 * @param len Length in bytes
 * @param alloc Allocator the value's context draws from, or NULL for the
 *   default
 * @return New value the caller releases with gtext_json_free(), or NULL
 */
GTEXT_INTERNAL_API GTEXT_JSON_Value * json_value_new_string_on(
    const char * s, size_t len, const GTEXT_Allocator * alloc);

/**
 * @brief Deep equality comparison for JSON values
 *
 * Performs a deep structural comparison of two JSON values. Returns 1 if
 * the values are equal (same structure and content), 0 otherwise. Used by
 * patch test operations and schema validation for enum/const matching.
 *
 * For numbers, compares using the best available representation (int64, uint64,
 * double with epsilon, or lexeme). For objects, compares regardless
 * of key order.
 *
 * @param a First value to compare (can be NULL)
 * @param b Second value to compare (can be NULL)
 * @return 1 if values are equal, 0 otherwise
 */
/* Free the context of every descendant of @p v whose context differs from its
   parent's, deepest first.
 *
 * Not about reclaiming @p v's own nodes: those live in @p v's context's arena,
 * which gtext_json_free() releases in one go.  It is about subtrees grafted in
 * from *another* arena - which is what storing a separately created value does
 * - whose arenas nothing else will free.
 *
 * Shared with json_patch.c, where the atomic clone-and-copy-back in
 * gtext_json_patch_apply() and gtext_json_merge_patch() replaces the original's
 * content and so has to release whatever the original was holding. */
GTEXT_INTERNAL_API void json_free_foreign_contexts(GTEXT_JSON_Value * v);

int json_value_equal(const GTEXT_JSON_Value * a, const GTEXT_JSON_Value * b);

/**
 * @brief Generate a context snippet around an error position
 *
 * Generates a context snippet from the input buffer around the error position,
 * showing a window of text before and after the error. The snippet is useful
 * for displaying error messages with visual context.
 *
 * @param input Input buffer (must not be NULL)
 * @param input_len Length of input buffer
 * @param error_offset Byte offset of error in input (0-based)
 * @param context_before Number of bytes to include before error (default: 20)
 * @param context_after Number of bytes to include after error (default: 20)
 * @param snippet_out Output: pointer to allocated snippet (caller must free
 * with free())
 * @param snippet_len_out Output: length of snippet
 * @param caret_offset_out Output: byte offset of caret within snippet (0-based)
 * @return GTEXT_JSON_OK on success, GTEXT_JSON_E_OOM on allocation failure
 */
GTEXT_JSON_Status json_error_generate_context_snippet(const char * input,
    size_t input_len, size_t error_offset, size_t context_before,
    size_t context_after, char ** snippet_out, size_t * snippet_len_out,
    size_t * caret_offset_out);

/**
 * @brief Get token description for error reporting
 *
 * Returns a human-readable description of a token type for use in
 * error messages (e.g., "string", "number", "comma", "colon").
 *
 * @param token_type Token type from json_token_type enum
 * @return Static string describing the token, or "unknown token" if invalid
 */
const char * json_token_type_description(int token_type);

/**
 * @brief The text a keyword token was written with, or NULL if it is not one
 *
 * For the one place a keyword is a name rather than a value: JSON5's unquoted
 * object names are ECMAScript IdentifierNames, which include the reserved
 * words.
 *
 * @param token_type Token type from json_token_type enum
 * @return A static string, or NULL
 */
GTEXT_INTERNAL_API const char * json_keyword_token_spelling(int token_type);

/** The name a keyword token spells, or NULL if its bytes are not exactly the
 *  keyword - which is how a signed nonfinite is refused in name position. */
GTEXT_INTERNAL_API const char * json_keyword_token_name(
    const json_token * token);

/**
 * @brief Get effective limit value (use default if configured is 0)
 *
 * This utility function returns the effective limit value. If the configured
 * value is greater than 0, it returns that value. Otherwise, it returns the
 * default value. This is used throughout the JSON parser to handle optional
 * limit configurations.
 *
 * @param configured The configured limit value (0 means use default)
 * @param default_val The default limit value to use if configured is 0
 * @return The effective limit value (either configured or default)
 */
size_t json_get_limit(size_t configured, size_t default_val);

/**
 * @brief Safely update position offset, checking for overflow
 *
 * Updates the offset field of a position structure, checking for overflow
 * and saturating at SIZE_MAX if overflow would occur.
 *
 * @param pos Position structure to update (must not be NULL)
 * @param increment Number of bytes to add to offset
 */
void json_position_update_offset(json_position * pos, size_t increment);

/**
 * @brief Safely update position column, checking for overflow
 *
 * Updates the column field of a position structure, checking for integer
 * overflow and saturating at INT_MAX if overflow would occur.
 *
 * @param pos Position structure to update (must not be NULL)
 * @param increment Number of columns to add
 */
void json_position_update_column(json_position * pos, size_t increment);

/**
 * @brief Safely increment line number, checking for overflow
 *
 * Increments the line number in a position structure, checking for integer
 * overflow and saturating at INT_MAX if already at maximum.
 *
 * @param pos Position structure to update (must not be NULL)
 */
void json_position_increment_line(json_position * pos);

/**
 * @brief Advance position by bytes, handling newlines
 *
 * Advances the position by a specified number of bytes, updating offset
 * and column appropriately. If the input contains newlines, line numbers
 * are incremented and columns are reset appropriately.
 *
 * This function scans the input buffer to detect newlines and updates
 * position tracking accordingly. It handles both single-byte newlines (\n)
 * and multi-byte newlines (\r\n).
 *
 * @param pos Position structure to update (must not be NULL)
 * @param input Input buffer to scan for newlines (can be NULL if input_len is
 * 0)
 * @param input_len Number of bytes to advance
 * @param start_offset Starting offset in input buffer (for newline detection)
 */
void json_position_advance(json_position * pos, const char * input,
    size_t input_len, size_t start_offset);

/**
 * @brief Buffer growth strategy type
 */
typedef enum {
  JSON_BUFFER_GROWTH_SIMPLE, ///< Simple doubling strategy
  JSON_BUFFER_GROWTH_HYBRID ///< Hybrid: fixed increment for small, doubling for
                            ///< large
} json_buffer_growth_strategy;

/**
 * @brief Unified buffer growth function
 *
 * Grows a buffer to accommodate at least the needed size, using a configurable
 * growth strategy. Supports both simple doubling and hybrid growth strategies.
 *
 * The hybrid strategy uses:
 * - Fixed increment (64 bytes) for small buffers (< threshold)
 * - Exponential growth (doubling) for large buffers (>= threshold)
 *
 * The simple strategy always doubles the size (with optional headroom).
 *
 * All operations include overflow protection to prevent integer overflow.
 *
 * @param buffer Pointer to buffer pointer (will be updated on reallocation)
 * @param capacity Pointer to current capacity (will be updated)
 * @param needed Minimum size needed
 * @param strategy Growth strategy to use
 * @param initial_size Initial size to use if capacity is 0 (0 = use default 64)
 * @param small_threshold Threshold for hybrid strategy (0 = use default 1024)
 * @param growth_multiplier Multiplier for exponential growth (0 = use default
 * 2)
 * @param fixed_increment Fixed increment for hybrid small buffers (0 = use
 * default 64)
 * @param headroom Additional headroom to add after growth (0 = no headroom)
 * @param alloc Allocator the buffer belongs to, or NULL for the default one.
 *   It is a parameter rather than something the caller assigns afterwards,
 *   because the buffer this grows is released through whatever allocator the
 *   owner records - so a buffer grown here through one allocator and freed
 *   through another is exactly the mismatch csv_field_buffer_init() was
 *   changed to prevent.  Both callers are on the streaming parser's path and
 *   pass the stream's own allocator.
 * @return GTEXT_JSON_OK on success, GTEXT_JSON_E_OOM on failure
 */
GTEXT_JSON_Status json_buffer_grow_unified(char ** buffer, size_t * capacity,
    size_t needed, json_buffer_growth_strategy strategy, size_t initial_size,
    size_t small_threshold, size_t growth_multiplier, size_t fixed_increment,
    size_t headroom, const GTEXT_Allocator * alloc);

/**
 * @brief Check if addition would overflow (size_t)
 *
 * Returns true if adding b to a would cause an overflow.
 *
 * @param a First operand
 * @param b Second operand
 * @return 1 if overflow would occur, 0 otherwise
 */
int json_check_add_overflow(size_t a, size_t b);

/**
 * @brief Check if multiplication would overflow (size_t)
 *
 * Returns true if multiplying a by b would cause an overflow.
 *
 * @param a First operand
 * @param b Second operand
 * @return 1 if overflow would occur, 0 otherwise
 */
int json_check_mul_overflow(size_t a, size_t b);

/**
 * @brief Check if subtraction would underflow (size_t)
 *
 * Returns true if subtracting b from a would cause an underflow.
 *
 * @param a First operand (minuend)
 * @param b Second operand (subtrahend)
 * @return 1 if underflow would occur, 0 otherwise
 */
int json_check_sub_underflow(size_t a, size_t b);

/**
 * @brief Check if integer addition would overflow (int)
 *
 * Returns true if adding increment to current would cause an integer overflow.
 * Used for column/line tracking where values are int.
 *
 * @param current Current integer value
 * @param increment Value to add
 * @return 1 if overflow would occur, 0 otherwise
 */
int json_check_int_overflow(int current, size_t increment);

/**
 * @brief Validate a pointer parameter and optionally set error
 *
 * This helper function checks if a pointer is NULL and, if so, optionally
 * sets an error code and message if an error output structure is provided.
 * This is useful for parameter validation at function entry points.
 *
 * Note: Many NULL checks are context-specific and don't need this helper.
 * Use this only when the pattern matches: check NULL, set error if provided,
 * return error code.
 *
 * @param ptr Pointer to validate (can be NULL)
 * @param err Error output structure (can be NULL, in which case no error is
 * set)
 * @param error_code Error code to set if ptr is NULL
 * @param error_message Error message to set if ptr is NULL
 * @return 1 if ptr is NULL (error case), 0 if ptr is valid
 */
int json_check_null_param(const void * ptr, GTEXT_JSON_Error * err,
    GTEXT_JSON_Status error_code, const char * error_message);

/**
 * @brief Check if an array index is within bounds
 *
 * Returns true if the index is valid (within bounds) for an array of the given
 * size. An index is valid if it is less than the size.
 *
 * @param index Index to check
 * @param size Size of the array
 * @return 1 if index is in bounds (valid), 0 if out of bounds
 */
int json_check_bounds_index(size_t index, size_t size);

/**
 * @brief Check if a buffer offset is within bounds
 *
 * Returns true if the offset is valid (within bounds) for a buffer of the given
 * size. An offset is valid if it is less than the size.
 *
 * @param offset Offset to check
 * @param size Size of the buffer
 * @return 1 if offset is in bounds (valid), 0 if out of bounds
 */
int json_check_bounds_offset(size_t offset, size_t size);

/**
 * @brief Check if a pointer is within a range
 *
 * Returns true if the pointer is within the range [start, end) (start
 * inclusive, end exclusive). This is useful for validating pointer arithmetic
 * results.
 *
 * @param ptr Pointer to check
 * @param start Start of valid range (inclusive)
 * @param end End of valid range (exclusive)
 * @return 1 if ptr is in range, 0 if out of range
 */
int json_check_bounds_ptr(
    const void * ptr, const void * start, const void * end);

/**
 * @brief Initialize error structure fields to defaults
 *
 * Initializes an error structure to default values. This is useful for
 * setting up error structures before populating them with specific error
 * information. Note that this does NOT free any existing context snippet;
 * use gtext_json_error_free() first if needed.
 *
 * @param err Error structure to initialize (must not be NULL)
 * @param code Error code to set
 * @param message Error message to set
 * @param offset Byte offset of error
 * @param line Line number of error (1-based)
 * @param col Column number of error (1-based)
 */
void json_error_init_fields(GTEXT_JSON_Error * err, GTEXT_JSON_Status code,
    const char * message, size_t offset, int line, int col);

/**
 * @brief Check if a string length would overflow when adding null terminator
 *
 * This is a common validation pattern used when allocating buffers for strings.
 * Returns true if adding 1 (for null terminator) to the length would cause
 * an overflow. This is equivalent to checking `len > SIZE_MAX - 1`.
 *
 * @param len String length to check
 * @return 1 if overflow would occur, 0 otherwise
 */
int json_check_string_length_overflow(size_t len);

/**
 * @brief Parser state structure
 */
typedef struct {
  json_lexer lexer;                      ///< Lexer for tokenization
  const GTEXT_JSON_Parse_Options * opts; ///< Parse options
  size_t depth;                          ///< Current nesting depth
  size_t total_bytes_consumed;           ///< Total bytes processed
  GTEXT_JSON_Error * error_out;          ///< Error output structure
} json_parser;

/**
 * @brief Stack entry type for tracking nesting in writer
 */
typedef enum {
  JSON_WRITER_STACK_OBJECT,
  JSON_WRITER_STACK_ARRAY
} json_writer_stack_type;

/**
 * @brief Stack entry for tracking nesting in writer
 */
typedef struct {
  json_writer_stack_type type; ///< Object or array
  int has_elements;            ///< Whether any elements have been written
  int expecting_key; ///< For objects: 1 if expecting key, 0 if expecting value
} json_writer_stack_entry;

/**
 * @brief Streaming writer structure
 */
struct GTEXT_JSON_Writer {
  GTEXT_JSON_Sink sink;            ///< Output sink
  GTEXT_JSON_Write_Options opts;   ///< Write options (copy)
  json_writer_stack_entry * stack; ///< Stack for tracking nesting
  size_t stack_capacity;           ///< Stack capacity
  size_t stack_size;               ///< Current stack depth
  int error;                       ///< Error flag (1 if error occurred)

  /**
   * Top-level values begun so far.
   *
   * Counted because the second one is the question: with
   * GTEXT_JSON_Write_Options::records off it is refused, and with a framing
   * named it is a new record. Incremented where a value begins rather than
   * where one ends, because writer_begin_value() is the single place a
   * top-level position is recognised - a container's own members are at a
   * stack depth above zero and do not reach it.
   */
  size_t roots_written;

  /**
   * A record has been written whose terminator has not.
   *
   * The same deferral the CSV streaming writer makes, for the same reason: the
   * byte after a record is written either when the next record starts or at
   * gtext_json_writer_finish(), and only those two places know which. Always 0
   * when records is GTEXT_JSON_RECORDS_OFF.
   */
  int records_pending;
};

/**
 * @brief Property schema entry
 */
typedef struct {
  char * key;                       ///< Property name (allocated)
  size_t key_len;                   ///< Property name length
  struct json_schema_node * schema; ///< Schema for this property
} json_schema_property;

/**
 * @brief One entry of a `dependentRequired` map
 */
typedef struct {
  char * key;         ///< Property whose presence triggers the requirement
  char ** required;   ///< Property names that must then also be present
  size_t required_count; ///< Number of them
} json_schema_dep_required;

/**
 * @brief One entry of a `dependentSchemas` map
 */
typedef struct {
  char * key;                       ///< Property whose presence applies it
  struct json_schema_node * schema; ///< Schema the object must then satisfy
} json_schema_dep_schema;

/**
 * One `patternProperties` entry: a regular expression and the schema every
 * property whose name matches it must satisfy. Several entries can apply to
 * the same property, and then all of them do.
 */
typedef struct {
  void * regex;                     ///< The provider's compiled handle; owned
  struct json_schema_node * schema; ///< What a matching property must satisfy
} json_schema_pattern_property;

/**
 * @brief Compiled schema node
 */
typedef struct json_schema_node {
  /**
   * A boolean schema: `true` accepts everything, `false` rejects everything.
   * JSON Schema allows one anywhere a schema is allowed, and
   * `"additionalProperties": false` is the common case. When this is set the
   * rest of the node is unused.
   */
  int is_bool_schema;
  int bool_schema_value;

  /**
   * Target of `$ref`, or NULL. Borrowed, never owned: ref targets live in the
   * schema's registry so that a recursive or shared reference is compiled once
   * and freed once.
   */
  struct json_schema_node * ref_target;

  // Type validation
  unsigned int type_flags; ///< Bitmask of allowed types (0 = any type)

  // Object validation
  json_schema_property * properties; ///< Property schemas (NULL if none)
  size_t properties_count;           ///< Number of properties
  size_t properties_capacity;        ///< Allocated capacity

  char ** required_keys;    ///< Array of required property names (NULL if none)
  size_t required_count;    ///< Number of required keys
  size_t required_capacity; ///< Allocated capacity for required keys

  // Array validation
  struct json_schema_node *
      items_schema; ///< Schema for array items (NULL if none)

  // Enum/const validation
  /**
   * 1 if `enum` was given at all. Distinct from a non-zero enum_count,
   * because `"enum": []` is a schema that nothing satisfies rather than a
   * schema that asserts nothing.
   */
  int has_enum;
  GTEXT_JSON_Value **
      enum_values;      ///< Array of allowed enum values (NULL if none)
  size_t enum_count;    ///< Number of enum values
  size_t enum_capacity; ///< Allocated capacity for enum values
  GTEXT_JSON_Value * const_value; ///< Single const value (NULL if none)

  // Numeric constraints
  int has_minimum; ///< 1 if minimum is set
  double minimum;  ///< Minimum value (inclusive)
  int has_maximum; ///< 1 if maximum is set
  double maximum;  ///< Maximum value (inclusive)

  // String constraints
  int has_min_length; ///< 1 if minLength is set
  size_t min_length;  ///< Minimum string length
  int has_max_length; ///< 1 if maxLength is set
  size_t max_length;  ///< Maximum string length

  // Array constraints
  int has_min_items; ///< 1 if minItems is set
  size_t min_items;  ///< Minimum array size
  int has_max_items; ///< 1 if maxItems is set
  size_t max_items;  ///< Maximum array size

  int has_unique_items; ///< 1 if uniqueItems was given
  int unique_items;     ///< Its value

  // Numeric constraints beyond minimum/maximum
  int has_exclusive_minimum; ///< 1 if exclusiveMinimum is set
  double exclusive_minimum;  ///< Exclusive lower bound
  int has_exclusive_maximum; ///< 1 if exclusiveMaximum is set
  double exclusive_maximum;  ///< Exclusive upper bound
  /**
   * 1 when `integer` means draft-04's `integer`.
   *
   * draft-04 defines the primitive types by how a value is *written*, so a
   * number carrying a fraction or an exponent is a "number" and not an
   * "integer" - `1.0` is invalid against `{"type":"integer"}` there and valid
   * against it in every later draft, which changed the rule to be about the
   * value.
   */
  int integer_excludes_fraction;
  int has_multiple_of;       ///< 1 if multipleOf is set
  double multiple_of;        ///< Divisor; must be greater than zero
  /**
   * The divisor as the schema spelled it, when the schema kept its lexeme.
   * `multipleOf` is a decimal question and `multiple_of` above is a binary
   * approximation of the divisor, so the exact form is preferred whenever
   * both it and the instance have one.
   */
  json_decimal multiple_of_decimal;
  int has_multiple_of_decimal;

  // Object size constraints
  int has_min_properties; ///< 1 if minProperties is set
  size_t min_properties;  ///< Minimum number of properties
  int has_max_properties; ///< 1 if maxProperties is set
  size_t max_properties;  ///< Maximum number of properties

  // dependentRequired
  json_schema_dep_required * dep_required; ///< Entries (NULL if none)
  size_t dep_required_count;               ///< Number of entries

  // Boolean applicators.  Each is a list of subschemas, or a single one.
  struct json_schema_node ** all_of; ///< Every one must match
  size_t all_of_count;
  struct json_schema_node ** any_of; ///< At least one must match
  size_t any_of_count;
  struct json_schema_node ** one_of; ///< Exactly one must match
  size_t one_of_count;
  struct json_schema_node * not_schema; ///< Must NOT match

  // if / then / else.  `if` alone asserts nothing; it selects which of the
  // other two applies.
  struct json_schema_node * if_schema;
  struct json_schema_node * then_schema;
  struct json_schema_node * else_schema;

  // Positional array schemas: 2020-12 spells this prefixItems, draft-07
  // spells it `items` with an array value. Both compile to this.
  struct json_schema_node ** prefix_items;
  size_t prefix_items_count;
  /// Applies to items beyond prefix_items. 2020-12 spells it `items`,
  /// draft-07 `additionalItems`.
  struct json_schema_node * additional_items;

  // contains / minContains / maxContains
  struct json_schema_node * contains_schema;
  int has_min_contains;
  size_t min_contains;
  int has_max_contains;
  size_t max_contains;

  /// Applies to properties that `properties` did not name.
  struct json_schema_node * additional_properties;
  /// Applies to each property name, as a string instance.
  struct json_schema_node * property_names;

  /**
   * Which schema resource this node's schema object belongs to, as one plus
   * its index in the schema's resource table, or 0 for none.
   *
   * One plus, because a node is allocated with calloc and index 0 is a real
   * resource - the document itself. Only `$dynamicRef` reads this, and only
   * to know when validation has crossed from one resource into another.
   */
  size_t resource_slot;

  /**
   * The plain-name fragment of a `$dynamicRef`, or NULL. Owned.
   *
   * Set only when the reference is the kind that behaves dynamically: a
   * plain name whose statically-resolved target carries a `$dynamicAnchor`
   * of that name. Every other `$dynamicRef` is a `$ref` with a longer
   * spelling, and compiles to ref_target alone.
   */
  char * dynamic_ref_name;
  size_t dynamic_ref_name_len;

  /**
   * The schema this node belongs to, for the one keyword that has to look
   * something up at validation time. Borrowed, and never null on a node that
   * carries a dynamic_ref_name.
   */
  const struct GTEXT_JSON_Schema * owner;

  /**
   * `unevaluatedItems` and `unevaluatedProperties`.
   *
   * These are the only keywords that need to know what the *rest* of this
   * schema object did. Everything else can be decided from the instance and
   * its own value; these two apply to whatever no other keyword in scope
   * reached, so validation has to carry that around and hand it to them.
   */
  struct json_schema_node * unevaluated_items;
  struct json_schema_node * unevaluated_properties;

  /**
   * The `format` this node asserts, or NULL. Owned, and set only under
   * GTEXT_JSON_FORMAT_ASSERT - in the default annotation policy the keyword
   * constrains nothing and nothing is kept.
   */
  char * format_name;
  size_t format_name_len;

  // dependentSchemas
  json_schema_dep_schema * dep_schemas;
  size_t dep_schemas_count;

  /**
   * The provider the two regular-expression keywords were compiled with, or
   * NULL if this node has none. Borrowed from the schema, which holds the
   * copy; it is kept per node so that json_schema_node_free() can release the
   * handles below without threading the schema through every recursive call.
   */
  const GTEXT_JSON_Regex_Provider * regex_provider;

  /**
   * `pattern`: the compiled handle, owned.
   *
   * The pattern *text* is not kept. It was, briefly, "so that an error could
   * name it" - and nothing ever did, because a compile failure already
   * carries the provider's message and an offset into the pattern, and a
   * match failure would have to allocate on a path that allocates nothing
   * today. A field kept for a use that does not exist is a field that will be
   * wrong by the time one does.
   */
  void * pattern_regex;

  /// `patternProperties`, in document order.
  json_schema_pattern_property * pattern_properties;
  size_t pattern_properties_count;
} json_schema_node;

/**
 * @brief Schema type flags
 */
typedef enum {
  JSON_SCHEMA_TYPE_NULL = 1,
  JSON_SCHEMA_TYPE_BOOL = 2,
  JSON_SCHEMA_TYPE_NUMBER = 4,
  JSON_SCHEMA_TYPE_STRING = 8,
  JSON_SCHEMA_TYPE_ARRAY = 16,
  JSON_SCHEMA_TYPE_OBJECT = 32,
  /// JSON Schema's "integer": a number with nothing after the decimal point.
  /// JSON itself has one number type, so this is a constraint on the value
  /// rather than a distinct instance type.
  JSON_SCHEMA_TYPE_INTEGER = 64
} json_schema_type_flags;

/**
 * @brief Compiled schema structure
 */
/**
 * @brief One resolved `$ref` target
 */
typedef struct {
  char * uri;              ///< Absolute URI it was reached by, owned
  json_schema_node * node; ///< Compiled schema, owned by the registry
} json_schema_ref_entry;

/**
 * @brief One `$dynamicAnchor`, and the resource it was declared in
 */
typedef struct {
  size_t resource_slot;    ///< One plus the resource's index
  char * name;             ///< The anchor name, owned
  json_schema_node * node; ///< The schema it names; owned elsewhere
  /**
   * The schema object it was found on, borrowed.
   *
   * Entries are created by the pre-pass, which runs over the whole document,
   * and only then compiled - because a `$dynamicAnchor` inside a `$defs`
   * nothing refers to still has to be findable. Compiling only what a `$ref`
   * reaches left exactly those out, and they are the ones the recursive
   * patterns depend on.
   */
  const GTEXT_JSON_Value * value;
} json_schema_dynamic_anchor;

/**
 * @brief One schema resource or named anchor
 *
 * A resource is what an `$id` creates: a document with its own base URI, from
 * which every `$ref` inside it is resolved. An anchor is a name for a
 * location inside one. Both are found by a pre-pass before anything is
 * compiled, because a `$ref` may name a resource that appears later in the
 * document than the reference does.
 */
typedef struct {
  char * uri;                     ///< Absolute URI, owned
  const GTEXT_JSON_Value * value; ///< The schema it names; borrowed
} json_schema_resource;

struct GTEXT_JSON_Schema {
  json_schema_node * root; ///< Root schema node
  json_context * ctx;      ///< Context for cloned enum/const values

  /**
   * The schema document, cloned so that `$ref` can be resolved against it
   * after the caller has freed theirs.
   */
  GTEXT_JSON_Value * doc;

  /**
   * Compiled `$ref` targets, keyed by pointer. Entries are registered before
   * their children compile, so a schema that refers to itself terminates;
   * they are owned here rather than by the referring node, so a target
   * reached from two places is compiled once and freed once.
   */
  json_schema_ref_entry * refs;
  size_t refs_count;
  size_t refs_capacity;

  /**
   * Every `$id` and `$anchor` reachable from the document, keyed by the
   * absolute URI it resolves to. Built before compilation starts: a `$ref`
   * is free to name something that appears later in the document than the
   * reference does, so resolving them as they are met would fail on a
   * forward reference and succeed on a backward one.
   */
  json_schema_resource * resources;
  size_t resources_count;
  size_t resources_capacity;

  /**
   * The base URI the document was compiled against, owned. Empty unless the
   * root carried an `$id`, which is the usual case.
   */
  char * base_uri;

  /**
   * Every `$dynamicAnchor`, with the resource each was declared in.
   *
   * A `$dynamicRef` is resolved against the chain of resources validation
   * has passed through rather than against the document, so this is looked
   * up at validation time and not at compile time - which is the whole
   * difference between it and `$ref`.
   */
  json_schema_dynamic_anchor * dynamic_anchors;
  size_t dynamic_anchors_count;
  size_t dynamic_anchors_capacity;

  /**
   * Meta-schema documents this library embeds, parsed on first use.
   *
   * A `$ref` that leaves the document is normally the caller's resolver to
   * answer, but the nine documents of the 2020-12 dialect are shipped here,
   * so a schema that validates another schema against its own dialect needs
   * no resolver and no network.
   *
   * One slot per embedded document, allocated whole the first time any of
   * them is reached and NULL for every one that is not; `embedded_count` is
   * the number of slots rather than the number parsed. They are owned for the
   * schema's lifetime rather than freed when the compile ends, because
   * `resources` borrows pointers into them.
   */
  GTEXT_JSON_Value ** embedded;
  size_t embedded_count;

  /**
   * The caller's regular-expression provider, copied at compile time, and
   * whether there was one. Nodes point at this copy rather than at the
   * caller's options structure, which they are not promised outlives them.
   * What the copy's `ctx` points at is still the caller's to keep alive.
   */
  GTEXT_JSON_Regex_Provider regex_provider;
  int has_regex_provider;
};

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_JSON_JSON_INTERNAL_H
