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
 * @file yaml_core.h
 * @brief Core types, status codes, and options for the YAML module.
 *
 * This header defines the public error/status codes, the error reporting
 * structure, the primary node/document forward declarations and the
 * parse/write option types and their default constructors.
 *
 * All public APIs in the YAML module return @ref GTEXT_YAML_Status where
 * 0 indicates success (GTEXT_YAML_OK) and non-zero values indicate
 * different failure modes.
 */

#ifndef GHOTI_IO_GTEXT_YAML_YAML_CORE_H
#define GHOTI_IO_GTEXT_YAML_YAML_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_YAML_Status
 * @brief Status codes returned by YAML APIs.
 *
 * - GTEXT_YAML_OK: Success.
 * - GTEXT_YAML_E_INVALID: Generic parse/validation error.
 * - GTEXT_YAML_E_OOM: Out of memory.
 * - GTEXT_YAML_E_LIMIT: A configured limit was exceeded.
 * - GTEXT_YAML_E_DEPTH: Maximum nesting depth exceeded.
 * - GTEXT_YAML_E_INCOMPLETE: More input required to complete parsing.
 * - GTEXT_YAML_E_BAD_TOKEN: Unexpected token encountered by the scanner.
 * - GTEXT_YAML_E_BAD_ESCAPE: Invalid escape sequence in a quoted scalar.
 * - GTEXT_YAML_E_DUPKEY: Duplicate mapping key (policy may vary by options).
 * - GTEXT_YAML_E_WRITE: Sink/write error during serialization.
 * - GTEXT_YAML_E_STATE: Operation not valid in current parser/writer state.
 */
typedef enum {
  GTEXT_YAML_OK = 0,
  GTEXT_YAML_E_INVALID,
  GTEXT_YAML_E_OOM,
  GTEXT_YAML_E_LIMIT,
  GTEXT_YAML_E_DEPTH,
  GTEXT_YAML_E_INCOMPLETE,
  GTEXT_YAML_E_BAD_TOKEN,
  GTEXT_YAML_E_BAD_ESCAPE,
  GTEXT_YAML_E_DUPKEY,
  GTEXT_YAML_E_WRITE,
  GTEXT_YAML_E_STATE
} GTEXT_YAML_Status;

/**
 * @page gtext_yaml_offsets What an offset counts
 *
 * Bytes of the **decoded** character stream, not of the buffer you passed in.
 * The two are the same number for UTF-8 with no byte order mark, and for
 * nothing else: a mark is stripped before decoding and moves every offset by
 * three, and UTF-16 and UTF-32 bear no byte-for-byte relation to it at all.
 *
 * `line` and `col` are counted in characters and are right in every encoding,
 * so they are what to locate something by when the input was not plain UTF-8.
 * Translating the offsets back would mean keeping a map of the whole document
 * - a cost on every parse for something few callers need - so it is said here
 * instead.
 */

/**
 * @struct GTEXT_YAML_Error
 * @brief Rich error payload returned/filled by YAML operations.
 *
 * Fields:
 * - code: status code (see @ref GTEXT_YAML_Status).
 * - message: human-readable message (owned by caller or static; do not free).
 * - offset/line/col: where in the input the error is. See
 *   @ref gtext_yaml_offsets for what offset counts - it is not always
 *   an index into the buffer you passed in.
 * - context_snippet: optional heap-allocated snippet to show nearby input; freed by gtext_yaml_error_free().
 * - caret_offset: position within context_snippet of the error location.
 * - expected_token/actual_token: optional textual tokens to aid diagnostics.
 */
typedef struct {
  GTEXT_YAML_Status code;
  const char * message;
  size_t offset;
  int line;
  int col;
  char * context_snippet;
  size_t context_snippet_len;
  size_t caret_offset;
  const char * expected_token;
  const char * actual_token;
} GTEXT_YAML_Error;

/**
 * @struct GTEXT_YAML_Source_Location
 * @brief Source location metadata for a node.
 *
 * @ref gtext_yaml_offsets applies to @c offset.
 */
typedef struct {
  size_t offset;
  int line;
  int col;
} GTEXT_YAML_Source_Location;

/**
 * @enum GTEXT_YAML_Node_Type
 * @brief Node types present in the YAML DOM.
 */
typedef enum {
  GTEXT_YAML_NULL,
  GTEXT_YAML_BOOL,
  GTEXT_YAML_INT,
  GTEXT_YAML_FLOAT,
  GTEXT_YAML_STRING,
  GTEXT_YAML_SEQUENCE,
  GTEXT_YAML_MAPPING,
  GTEXT_YAML_SET,
  GTEXT_YAML_OMAP,
  GTEXT_YAML_PAIRS,
  GTEXT_YAML_ALIAS
} GTEXT_YAML_Node_Type;

typedef struct GTEXT_YAML_Node GTEXT_YAML_Node;
typedef struct GTEXT_YAML_Document GTEXT_YAML_Document;
typedef struct GTEXT_JSON_Value GTEXT_JSON_Value;

/**
 * @enum GTEXT_YAML_Dupkey_Mode
 * @brief What to do about a mapping whose keys are not unique.
 *
 * Keys have to be unique (3.2.1.3), so ERROR is the default and the other
 * three are the caller deciding to accept a document the spec does not allow.
 *
 * KEEP_ALL is the one that keeps the document: FIRST_WINS and LAST_WINS
 * remove a pair, and ERROR removes the whole parse.  A tool that has to see
 * the document as it was written - a linter, a formatter, an event stream -
 * needs the pairs it actually holds, duplicates and all, and
 * gtext_yaml_mapping_get() then answers with the first of them.  It also
 * turns off the JSON fast path, which has no way to represent such a
 * mapping.
 */
typedef enum {
  GTEXT_YAML_DUPKEY_ERROR,
  GTEXT_YAML_DUPKEY_FIRST_WINS,
  GTEXT_YAML_DUPKEY_LAST_WINS,
  GTEXT_YAML_DUPKEY_KEEP_ALL
} GTEXT_YAML_Dupkey_Mode;

/**
 * @enum GTEXT_YAML_Schema
 * @brief Implicit typing schema selection.
 */
typedef enum {
  GTEXT_YAML_SCHEMA_FAILSAFE,
  GTEXT_YAML_SCHEMA_JSON,
  GTEXT_YAML_SCHEMA_CORE
} GTEXT_YAML_Schema;

/**
 * @enum GTEXT_YAML_Mode
 * @brief Parse mode presets that override option defaults.
 */
typedef enum {
  GTEXT_YAML_MODE_DEFAULT,
  GTEXT_YAML_MODE_CONFIG
} GTEXT_YAML_Mode;

/**
 * @enum GTEXT_YAML_Scalar_Style
 * @brief Preferred scalar style for YAML emission.
 */
typedef enum {
  GTEXT_YAML_SCALAR_STYLE_PLAIN,
  GTEXT_YAML_SCALAR_STYLE_SINGLE_QUOTED,
  GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED,
  GTEXT_YAML_SCALAR_STYLE_LITERAL,
  GTEXT_YAML_SCALAR_STYLE_FOLDED
} GTEXT_YAML_Scalar_Style;

/**
 * @enum GTEXT_YAML_Flow_Style
 * @brief Preferred collection style for YAML emission.
 */
typedef enum {
  GTEXT_YAML_FLOW_STYLE_AUTO,
  GTEXT_YAML_FLOW_STYLE_BLOCK,
  GTEXT_YAML_FLOW_STYLE_FLOW
} GTEXT_YAML_Flow_Style;

/**
 * @enum GTEXT_YAML_Warning_Code
 * @brief Warning codes for non-fatal issues.
 */
typedef enum {
  GTEXT_YAML_WARNING_YAML11_BOOL,
  GTEXT_YAML_WARNING_YAML11_OCTAL,
  GTEXT_YAML_WARNING_YAML11_SEXAGESIMAL,
  GTEXT_YAML_WARNING_DUPLICATE_KEY,
  /**
   * A "%YAML" directive naming a 1.x version this parser does not implement,
   * such as "%YAML 1.7". Section 6.8.1 has the processor go on and parse the
   * document as its own version while saying so, on the grounds that the
   * minor versions are meant to stay compatible. A *major* version it does
   * not know is refused outright instead, and is an error rather than this.
   */
  GTEXT_YAML_WARNING_YAML_VERSION
} GTEXT_YAML_Warning_Code;

/**
 * @struct GTEXT_YAML_Warning
 * @brief Warning payload for non-fatal parse issues.
 *
 * @ref gtext_yaml_offsets applies to @c offset.
 */
typedef struct {
  GTEXT_YAML_Warning_Code code;
  const char * message;
  size_t offset;
  int line;
  int col;
} GTEXT_YAML_Warning;

/**
 * @brief Warning callback invoked for non-fatal issues.
 */
typedef void (*GTEXT_YAML_Warning_Callback)(
  const GTEXT_YAML_Warning * warning,
  void * user
);

#define GTEXT_YAML_WARNING_MASK(code) (1u << (code))

/**
 * @enum GTEXT_YAML_Encoding
 * @brief Output encoding selection for YAML emission.
 */
typedef enum {
  GTEXT_YAML_ENCODING_UTF8,
  GTEXT_YAML_ENCODING_UTF16LE,
  GTEXT_YAML_ENCODING_UTF16BE,
  GTEXT_YAML_ENCODING_UTF32LE,
  GTEXT_YAML_ENCODING_UTF32BE
} GTEXT_YAML_Encoding;

/**
 * @brief Custom tag constructor callback.
 */
typedef GTEXT_YAML_Status (*GTEXT_YAML_Custom_Tag_Constructor)(
  GTEXT_YAML_Document * doc,
  GTEXT_YAML_Node * node,
  const char * tag,
  void * user,
  GTEXT_YAML_Error * out_err
);

/**
 * @brief Custom tag representer callback.
 */
typedef GTEXT_YAML_Status (*GTEXT_YAML_Custom_Tag_Representer)(
  const GTEXT_YAML_Node * node,
  const char * tag,
  void * user,
  const char ** out_tag,
  GTEXT_YAML_Error * out_err
);

/**
 * @brief Custom tag JSON conversion callback.
 */
typedef GTEXT_YAML_Status (*GTEXT_YAML_Custom_Tag_Json_Converter)(
  const GTEXT_YAML_Node * node,
  const char * tag,
  void * user,
  GTEXT_JSON_Value ** out_json,
  GTEXT_YAML_Error * out_err
);

/**
 * @brief Custom tag handler registration.
 */
typedef struct {
  const char * tag;
  GTEXT_YAML_Custom_Tag_Constructor construct;
  GTEXT_YAML_Custom_Tag_Representer represent;
  GTEXT_YAML_Custom_Tag_Json_Converter to_json;
  void * user;
} GTEXT_YAML_Custom_Tag;

/**
 * @struct GTEXT_YAML_Parse_Options
 * @brief Options that control parsing behavior and limits.
 *
 * All size limits use 0 to denote "use the library default".
 */
typedef struct {
  /* Limits and behavior */
  GTEXT_YAML_Mode mode;
  GTEXT_YAML_Dupkey_Mode dupkeys;
  GTEXT_YAML_Schema schema;
  /* Maximum nesting depth, for reading a document and for walking one.
   *
   * It bounds the parser, the DOM writer and gtext_yaml_node_clone(). The
   * last two matter because the DOM constructors do not consult it: they
   * have no parent pointers, so asking a node how deep it sits would cost a
   * walk on every append, and a document built through the API can therefore
   * nest as far as memory allows. The limit is spent where the depth is
   * already known, which is on the way back out.
   *
   * A value of SIZE_MAX removes the limit, and removing it is safe: none of
   * these walks uses the C stack. They used to - roughly 344 bytes a level
   * while resolving, 228 in the DOM writer and 113 in the clone - so
   * SIZE_MAX was a way to ask for a segmentation fault and get one, at about
   * 24,000 levels resolving, 37,000 writing and 74,000 cloning. Each keeps
   * its stack on the heap now, so depth costs memory rather than a frame and
   * this limit is a policy rather than a guard rail. */
  size_t max_depth;
  size_t max_total_bytes;
  size_t max_alias_expansion;

  /* Toggles */
  bool validate_utf8;
  bool resolve_tags;
  bool retain_comments;
  bool yaml_1_1;
  bool enable_custom_tags;
  /* Whether application-defined tags are accepted: a local tag such as
   * "!point", or a global one under a prefix of your own.  These are valid
   * YAML and the specification's own examples use them, so the default is
   * true; false is a lockdown for input you do not trust.
   *
   * It does not govern the "tag:yaml.org,2002:" namespace.  A tag there
   * that names no type the specification defines - "!!bogus" - is a
   * malformed document and is refused whatever this is set to. */
  bool allow_nonstandard_tags;
  bool allow_aliases;
  bool allow_merge_keys;

  /* Whether a mapping key may be a collection.  YAML allows it - "? [a, b]"
   * names a sequence as a key - and many consumers cannot represent one, so
   * false refuses the document rather than handing on something they will
   * mishandle.
   *
   * This did nothing at all until it was tested: the check that reads it
   * returned early on exactly the setting that was meant to switch it on. */
  bool allow_complex_keys;

  /* Whether a mapping key must resolve to a string.  Stricter than
   * allow_complex_keys, and refuses everything that would refuse - an
   * integer or boolean key is a scalar, so it passes the complex-key test
   * and fails this one.  JSON has no other kind of key, which is what this
   * is for. */
  bool require_string_keys;
  bool enable_json_fast_path;
  const GTEXT_YAML_Custom_Tag * custom_tags;
  size_t custom_tag_count;

  /* Warnings */
  GTEXT_YAML_Warning_Callback warning_callback;
  void * warning_user_data;
  bool warnings_as_errors;
  unsigned int warning_mask;

  /**
   * Allocator for everything the parse produces, or NULL for
   * gtext_allocator_default().
   *
   * The document records it, so gtext_yaml_free() releases through the same
   * allocator without the caller passing it again. It must stay valid for the
   * lifetime of the document the parse returns.
   *
   * Covered: every parse entry point - gtext_yaml_parse(),
   * gtext_yaml_parse_all(), gtext_yaml_parse_file() and the streaming parser,
   * the pull reader and the scanner behind them - the arena and every node,
   * scalar, key, anchor, tag and comment in it, the alias table, and the
   * parser's and scanner's own transient buffers. Also the DOM manipulation
   * functions afterwards, each using the allocator of the document it works
   * on, and gtext_yaml_to_json() for what it builds.
   *
   * Not covered, and still using the C library, exactly as in JSON and CSV:
   *
   * - GTEXT_YAML_Error and the strings it owns, released by
   *   gtext_yaml_error_free(), which is handed an error and no allocator.
   * - Nothing else. The writer is covered too, but by its own option:
   *   GTEXT_YAML_Write_Options::allocator, which a write is given rather than
   *   inheriting from the document - a document may be written to a sink that
   *   outlives it, and the two choices are the caller's to make separately.
   *
   * Neither is ever freed through a caller's allocator or vice versa, so there
   * is no path on which the two mix. `make check-allocators` is what keeps that
   * true.
   */
  const GTEXT_Allocator * allocator;
} GTEXT_YAML_Parse_Options;

/**
 * @struct GTEXT_YAML_Write_Options
 * @brief Options controlling document emission/serialization.
 */
typedef struct {
  bool pretty;
  int indent_spaces;
  int line_width;
  const char * newline;
  bool trailing_newline;
  bool canonical;
  GTEXT_YAML_Scalar_Style scalar_style;
  GTEXT_YAML_Flow_Style flow_style;
  GTEXT_YAML_Encoding encoding;
  bool emit_bom;
  bool enable_custom_tags;
  const GTEXT_YAML_Custom_Tag * custom_tags;
  size_t custom_tag_count;

  /* The dialect the output is meant to be read back in.
   *
   * Only a plain scalar is resolved by its contents (10.3.2), so whether a
   * scalar may go out plain is a question about what *that* schema and
   * version resolve - and the writer had no way to be told, so it answered
   * with the 1.2 core schema always. Every other dialect then read something
   * back that was not written:
   *
   *   the string "yes"  written plain, read back under 1_1   the bool true
   *   the string "012"  written plain, read back under 1_1   the integer 10
   *   a null            written "~",   read back under JSON  the string "~"
   *
   * These default to the 1.2 core schema, which is what the writer has always
   * emitted and what @ref gtext_yaml_parse_options_default reads. Set them to
   * the parse options a document came from - or will go to - and a string
   * that spells one of that dialect's words is quoted, and a null is spelled
   * the way that dialect spells one.
   *
   * @ref GTEXT_YAML_SCHEMA_FAILSAFE resolves nothing, so nothing needs
   * quoting for its sake; by the same token a document written for it cannot
   * carry a type at all, which is what asking for the failsafe schema means.
   */
  GTEXT_YAML_Schema schema;
  bool yaml_1_1;

  /**
   * The allocator the write's own working memory comes from, or NULL for the
   * default. **Not the sink's**: a buffer sink owns its buffer, because a sink
   * is created before any options are seen and outlives the write.
   *
   * What this covers is the scratch a write needs and then releases - the
   * frame stack that `gtext_yaml_write_document()` carries instead of
   * recursing - plus, for the event API, the ::GTEXT_YAML_Writer handle, its
   * node stack and the %TAG handles it records for the document being
   * written, which gtext_yaml_writer_new() allocates and
   * gtext_yaml_writer_free() releases through this same allocator. So an arena
   * caller writing a large document no longer reaches the C heap for any of
   * it.
   *
   * The same line GTEXT_JSON_Write_Options::allocator and
   * GTEXT_INI_Write_Options::allocator draw, and for the same reason: a
   * partial allocator is worse than none, so the sink's buffer is exempt
   * deliberately and documented rather than left to be discovered.
   *
   * Appended rather than placed with the formatting fields so that the offset
   * of every field that was already here stays where it was.
   */
  const GTEXT_Allocator * allocator;
} GTEXT_YAML_Write_Options;

/**
 * @brief Return a parse options object initialized to sensible defaults.
 *
 * The returned struct is by-value; callers may modify fields before
 * passing to APIs that accept @ref GTEXT_YAML_Parse_Options *.
 */
GTEXT_API GTEXT_YAML_Parse_Options gtext_yaml_parse_options_default(void);

/**
 * @brief Return parse options configured for safe-mode parsing.
 */
GTEXT_API GTEXT_YAML_Parse_Options gtext_yaml_parse_options_safe(void);

/**
 * @brief Return write options initialized to sensible defaults.
 */
GTEXT_API GTEXT_YAML_Write_Options gtext_yaml_write_options_default(void);

/**
 * @brief Free any heap owned members inside @p err and zero the structure.
 *
 * Safe to call with a NULL pointer.
 */
GTEXT_API void gtext_yaml_error_free(GTEXT_YAML_Error * err);

/**
 * @brief Free a parsed YAML document and all associated memory.
 *
 * After this call the @p doc pointer must not be used. Passing NULL is a no-op.
 */
GTEXT_API void gtext_yaml_free(GTEXT_YAML_Document * doc);

/**
 * @brief Parse a YAML file into a document.
 */
GTEXT_API GTEXT_YAML_Document * gtext_yaml_parse_file(
  const char * path,
  const GTEXT_YAML_Parse_Options * options,
  GTEXT_YAML_Error * out_err
);

/**
 * @brief Parse a YAML file into an array of documents.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_parse_file_all(
  const char * path,
  const GTEXT_YAML_Parse_Options * options,
  GTEXT_YAML_Document *** out_docs,
  size_t * out_count,
  GTEXT_YAML_Error * out_err
);

/**
 * @brief Write a YAML document to a file.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_write_file(
  const char * path,
  const GTEXT_YAML_Document * doc,
  const GTEXT_YAML_Write_Options * options,
  GTEXT_YAML_Error * out_err
);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_YAML_YAML_CORE_H
