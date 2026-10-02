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
 * Internal definitions for CSV module implementation.
 *
 * This header contains internal-only definitions used by the CSV module
 * implementation. It should not be included by external code.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSV_CSV_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSV_CSV_INTERNAL_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/text/csv/csv_core.h>
#include <ghoti.io/text/csv/csv_writer.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct GTEXT_CSV_Stream GTEXT_CSV_Stream;

/**
 * @brief Default limits for CSV parsing (used when opts->max_* is 0)
 */
#define CSV_DEFAULT_MAX_ROWS (10 * 1000 * 1000)          // 10M rows
#define CSV_DEFAULT_MAX_COLS (100 * 1000)                // 100k columns
#define CSV_DEFAULT_MAX_FIELD_BYTES (16 * 1024 * 1024)   // 16MB
#define CSV_DEFAULT_MAX_RECORD_BYTES (64 * 1024 * 1024)  // 64MB
#define CSV_DEFAULT_MAX_TOTAL_BYTES (1024 * 1024 * 1024) // 1GB

/**
 * @brief Default context radius for error snippets
 */
#define CSV_DEFAULT_CONTEXT_RADIUS_BYTES 40

/**
 * @brief Position tracking structure for CSV processing
 */
typedef struct {
  size_t offset; ///< Byte offset from start
  int line;      ///< Line number (1-based)
  int column;    ///< Column number (1-based, byte-based)
} csv_position;

/**
 * @brief Newline type detected
 */
typedef enum {
  CSV_NEWLINE_NONE, ///< No newline detected
  CSV_NEWLINE_LF,   ///< LF (\n)
  CSV_NEWLINE_CRLF, ///< CRLF (\r\n)
  CSV_NEWLINE_CR    ///< CR (\r)
} csv_newline_type;

/**
 * @brief UTF-8 validation result
 */
typedef enum {
  CSV_UTF8_VALID,     ///< Valid UTF-8 sequence
  CSV_UTF8_INVALID,   ///< Invalid UTF-8 sequence
  CSV_UTF8_INCOMPLETE ///< Incomplete UTF-8 sequence (needs more bytes)
} csv_utf8_result;

/**
 * @brief Detect and consume newline from input
 *
 * Detects newline type according to dialect settings and advances position.
 * Supports LF, CRLF, and optionally CR.
 *
 * @param input Input buffer
 * @param input_len Length of input buffer
 * @param pos Current position (updated on success)
 * @param dialect Dialect configuration
 * @return CSV_NEWLINE_* type if newline detected, CSV_NEWLINE_NONE otherwise
 */
GTEXT_INTERNAL_API csv_newline_type csv_detect_newline(const char * input,
    size_t input_len, csv_position * pos, const GTEXT_CSV_Dialect * dialect,
    GTEXT_CSV_Status * error_out);

/**
 * @brief Validate UTF-8 sequence
 *
 * Validates UTF-8 encoding in a byte sequence. Returns validation result
 * and optionally advances position.
 *
 * @param input Input buffer
 * @param input_len Length of input buffer
 * @param pos Current position (updated on success)
 * @param validate Whether to validate (if false, always returns VALID)
 * @return CSV_UTF8_VALID, CSV_UTF8_INVALID, or CSV_UTF8_INCOMPLETE
 */
GTEXT_INTERNAL_API csv_utf8_result csv_validate_utf8(const char * input,
    size_t input_len, csv_position * pos, bool validate,
    GTEXT_CSV_Status * error_out);

/**
 * @brief Strip UTF-8 BOM from input
 *
 * Checks if input starts with UTF-8 BOM (0xEF 0xBB 0xBF) and strips it if
 * present. Updates position accordingly.
 *
 * @param input Input buffer
 * @param input_len Length of input buffer (updated if BOM found)
 * @param pos Current position (updated if BOM found)
 * @param strip Whether to strip BOM (if false, does nothing)
 * @return true if BOM was stripped, false otherwise
 */
GTEXT_INTERNAL_API GTEXT_CSV_Status csv_strip_bom(const char ** input,
    size_t * input_len, csv_position * pos, bool strip, bool * was_stripped);

/**
 * @brief Arena block structure (internal)
 *
 * WARNING: This struct must NEVER be passed by value or copied.
 * It uses a flexible array member pattern and must always be used
 * as a pointer. Copying will only copy the struct header, not the
 * allocated data.
 */
typedef struct csv_arena_block {
  struct csv_arena_block * next; ///< Next block in the arena
  size_t used;                   ///< Bytes used in this block
  size_t size;                   ///< Total size of this block
  char data[1]; ///< Flexible array member for block data (C99 FAM, use data[1]
                ///< for C++ compatibility)
} csv_arena_block;

/**
 * @brief Arena allocator structure
 *
 * Manages a collection of blocks for efficient bulk allocation.
 * All memory is freed when the arena is destroyed.
 */
struct csv_arena {
  csv_arena_block * first;   ///< First block in the arena
  csv_arena_block * current; ///< Current block being used
  size_t block_size;         ///< Size of each new block
  /// The allocator every block came from, and the one they must go back to.
  /// Held here rather than only on the context because csv_arena_free() has
  /// to release the blocks after the context that named the allocator is
  /// already gone in some paths - and freeing a block through the wrong
  /// allocator corrupts the heap rather than leaking.
  const GTEXT_Allocator * alloc;
};

/**
 * @brief Typedef for csv_arena
 */
typedef struct csv_arena csv_arena;

/**
 * @brief Set error structure with common defaults
 *
 * Macro to standardize error initialization. Sets code, message, line, and
 * column. Defaults line and column to 1. All other fields are zero-initialized.
 * Additional fields can be set after the macro call if needed.
 *
 * @param err Error structure pointer (can be NULL, in which case this is a
 * no-op)
 * @param err_code Error code (GTEXT_CSV_Status)
 * @param err_msg Error message (const char *)
 */
#define CSV_SET_ERROR(err, err_code, err_msg)                                  \
  do {                                                                         \
    if (err) {                                                                 \
      *(err) = (GTEXT_CSV_Error){                                              \
          .code = (err_code), .message = (err_msg), .line = 1, .column = 1};   \
    }                                                                          \
  } while (0)

/**
 * @brief CSV context structure
 *
 * Holds the arena allocator and other context information
 * for a CSV table.
 */
typedef struct csv_context {
  csv_arena * arena; ///< Arena allocator for this table
  const char *
      input_buffer; ///< Original input buffer (for in-situ mode, caller-owned)
  size_t input_buffer_len; ///< Length of input buffer (for in-situ mode)
  /// The caller's allocator, or NULL for gtext_allocator_default(). Every
  /// allocation a table owns is reachable from here, which is what makes
  /// `table->ctx->alloc` the answer at almost every site in csv_table.c.
  const GTEXT_Allocator * alloc;
} csv_context;

/**
 * @brief Create a new CSV context with arena
 *
 * Internal function for creating a new context with an arena allocator.
 *
 * @return New context, or NULL on failure
 */
GTEXT_INTERNAL_API csv_context * csv_context_new(
    const GTEXT_Allocator * alloc);

/**
 * @brief Set input buffer for in-situ mode
 *
 * Internal function for storing a reference to the input buffer in the context.
 * The buffer is caller-owned and must remain valid for the lifetime of the
 * table.
 *
 * @param ctx Context to set input buffer on (must not be NULL)
 * @param input_buffer Original input buffer (caller-owned, must remain valid)
 * @param input_buffer_len Length of input buffer
 */
GTEXT_INTERNAL_API void csv_context_set_input_buffer(
    csv_context * ctx, const char * input_buffer, size_t input_buffer_len);

/**
 * @brief Free a CSV context and its arena
 *
 * Internal function for freeing a context and its associated arena.
 * Note: The input buffer (if set) is caller-owned and is NOT freed here.
 *
 * @param ctx Context to free (can be NULL)
 */
GTEXT_INTERNAL_API void csv_context_free(csv_context * ctx);

/**
 * @brief Allocate memory from a context's arena
 *
 * Internal function for allocating memory from the
 * arena associated with a context.
 *
 * @param ctx Context containing the arena
 * @param size Size in bytes to allocate
 * @param align Alignment requirement (must be power of 2)
 * @return Pointer to allocated memory, or NULL on failure
 */
GTEXT_INTERNAL_API void * csv_arena_alloc_for_context(
    csv_context * ctx, size_t size, size_t align);

/**
 * @brief Write a field with proper quoting and escaping
 *
 * Internal function for writing a field to a sink with appropriate
 * quoting and escaping according to write options and dialect.
 * This function handles:
 * - Quote-if-needed logic (delimiter, quote char, newline present)
 * - Quote-all and quote-empty options
 * - Escape mode: doubled quote vs backslash vs none
 *
 * @param sink Output sink
 * @param field_data Field data (may be NULL if field_len is 0)
 * @param field_len Field length in bytes
 * @param opts Write options
 * @return GTEXT_CSV_OK on success, error code on failure
 */
GTEXT_INTERNAL_API GTEXT_CSV_Status csv_write_field(const GTEXT_CSV_Sink * sink,
    const char * field_data, size_t field_len,
    const GTEXT_CSV_Write_Options * opts);

/**
 * @brief Set original input buffer for in-situ mode
 *
 * Internal function for setting the original input buffer in a stream.
 * This is used by table parsing to enable in-situ mode field references.
 *
 * @param stream Stream parser (must not be NULL)
 * @param input_buffer Original input buffer (caller-owned, must remain valid)
 * @param input_buffer_len Length of input buffer
 */
GTEXT_INTERNAL_API void csv_stream_set_original_input_buffer(
    GTEXT_CSV_Stream * stream, const char * input_buffer,
    size_t input_buffer_len);

/**
 * @brief Tell a stream that the leading BOM has already been dealt with.
 *
 * gtext_csv_parse_table() strips the BOM itself, because the buffer it hands
 * on is also what in-situ fields and error snippets point into.  Without this
 * the stream then looked for a BOM at the start of what it was given and
 * stripped a second one, so a document beginning with two BOMs lost both
 * through the table parser and one through the streaming parser.  Only the
 * first is a BOM; a second is a zero-width no-break space, which is data.
 */
GTEXT_INTERNAL_API void csv_stream_mark_bom_resolved(GTEXT_CSV_Stream * stream);

/**
 * @brief Generate a context snippet around an error position
 *
 * Extracts a snippet of text around the error position for better error
 * reporting. The snippet includes context before and after the error position,
 * with a caret offset indicating the exact error location.
 *
 * @param input Input buffer containing the CSV data
 * @param input_len Length of input buffer
 * @param error_offset Byte offset of the error (0-based)
 * @param context_before Number of bytes of context before the error
 * @param context_after Number of bytes of context after the error
 * @param snippet_out Output parameter for the allocated snippet (caller must
 * free)
 * @param snippet_len_out Output parameter for snippet length
 * @param caret_offset_out Output parameter for caret offset within snippet
 * @return GTEXT_CSV_OK on success, error code on failure
 */
GTEXT_INTERNAL_API GTEXT_CSV_Status csv_error_generate_context_snippet(
    const char * input, size_t input_len, size_t error_offset,
    size_t context_before, size_t context_after, char ** snippet_out,
    size_t * snippet_len_out, size_t * caret_offset_out);

/**
 * @brief Copy an error structure, deep-copying the context snippet
 *
 * Copies an error structure from source to destination, including a deep copy
 * of the context snippet if present. The destination's existing context snippet
 * (if any) is freed before copying.
 *
 * @param dst Destination error structure (must not be NULL)
 * @param src Source error structure (must not be NULL)
 * @return GTEXT_CSV_OK on success, error code on failure
 */
GTEXT_INTERNAL_API GTEXT_CSV_Status csv_error_copy(
    GTEXT_CSV_Error * dst, const GTEXT_CSV_Error * src);

// ============================================================================
// Memory Management Design Notes
// ============================================================================

/**
 * @brief Memory Allocation Strategy for CSV Table Operations
 *
 * The CSV module uses two distinct memory allocation strategies:
 *
 * **Arena Allocation (csv_arena_alloc_for_context):**
 * - Used for all permanent table data structures
 * - Row structures (csv_table_row)
 * - Field arrays (csv_table_field *)
 * - Field data (string content)
 * - Header map entries
 * - All data that lives with the table for its entire lifetime
 * - Freed in bulk when the table is destroyed
 *
 * **Malloc Allocation (malloc/free):**
 * - Used for temporary organizational arrays during mutation operations
 * - Temporary pointer arrays (e.g., csv_column_op_temp_arrays)
 * - Arrays used to organize/prepare data before committing to the table
 * - Freed immediately after each operation completes
 * - Not part of the permanent table structure
 *
 * **Rationale:**
 * Temporary arrays use malloc() instead of arena allocation because:
 * 1. **Lifetime**: They are freed immediately after use, not when the table is
 * destroyed
 * 2. **Memory efficiency**: Keeping temporary data in the arena would waste
 * memory until table destruction
 * 3. **Error handling**: Immediate cleanup on error paths is simpler and safer
 * 4. **Separation of concerns**: Temporary organizational data is distinct from
 *    permanent table structures
 *
 * This design maintains atomicity: all allocations (both arena and malloc)
 * happen before any state updates, allowing clean rollback on failure.
 */

// ============================================================================
// Table Structure Definitions (for internal use)
// ============================================================================

/**
 * @brief Field structure (stored in arena)
 */
typedef struct {
  const char * data; ///< Field data (pointer into input or arena)
  size_t length;     ///< Field length
  bool is_in_situ;   ///< Whether field references input buffer directly
} csv_table_field;

/**
 * @brief Row structure (stored in arena)
 */
typedef struct {
  csv_table_field * fields; ///< Array of fields
  size_t field_count;       ///< Number of fields
} csv_table_row;

/**
 * @brief Header map entry (for column name lookup)
 */
typedef struct csv_header_entry {
  const char * name;              ///< Column name (in arena or input buffer)
  size_t name_len;                ///< Column name length
  size_t index;                   ///< Column index
  struct csv_header_entry * next; ///< Next entry (for hash table chaining)
} csv_header_entry;

/**
 * @brief Temporary arrays for column operations
 *
 * Holds all temporary arrays allocated during column operations.
 * These arrays are allocated with malloc() and must be freed when done.
 *
 * @note See "Memory Management Design Notes" section in this file for
 *       detailed explanation of why temporary arrays use malloc() instead
 *       of arena allocation.
 */
typedef struct {
  csv_table_field **
      new_field_arrays;        ///< Array of pointers to new field arrays
  size_t * old_field_counts;   ///< Array of old field counts per row
  char ** field_data_array;    ///< Array of field data pointers (from
                               ///< csv_preallocate_column_field_data)
  size_t * field_data_lengths; ///< Array of field data lengths (from
                               ///< csv_preallocate_column_field_data)
  /// The allocator every array above came from. Carried in the structure
  /// so the cleanup paths reach it without a signature change, and so an
  /// unwind that runs before the context exists still frees correctly.
  const GTEXT_Allocator * alloc;
} csv_column_op_temp_arrays;

/**
 * @brief Structures for table compaction
 */
typedef struct {
  csv_context * new_ctx;               ///< New context with arena
  csv_table_row * new_rows;            ///< New rows array
  csv_table_field ** new_field_arrays; ///< Array of field arrays
  char *** new_field_data_ptrs;        ///< Array of field data pointer arrays
  /// The allocator every array above came from. Carried in the structure
  /// so the cleanup paths reach it without a signature change, and so an
  /// unwind that runs before the context exists still frees correctly.
  const GTEXT_Allocator * alloc;
} csv_compact_structures;

/**
 * @brief Table structure (internal)
 *
 * This is the internal representation of GTEXT_CSV_Table.
 * External code should use the opaque GTEXT_CSV_Table type.
 * This structure definition must match the one in csv_table.c.
 */
struct GTEXT_CSV_Table {
  csv_context * ctx;    ///< Context with arena
  csv_table_row * rows; ///< Array of rows
  size_t row_count;     ///< Number of rows
  size_t row_capacity;  ///< Allocated row capacity
  size_t column_count; ///< Expected column count (set by first row, 0 if empty)

  // Header map (optional, only if header processing enabled)
  csv_header_entry ** header_map; ///< Hash table for header lookup
  size_t header_map_size;         ///< Size of hash table
  bool has_header;                ///< Whether header was processed
  bool require_unique_headers;    ///< Whether to enforce unique headers for
                                  ///< mutation operations (default: false)
  bool allow_irregular_rows;      ///< Whether to allow irregular rows (rows
                                  ///< with different field counts) in mutation
                                  ///< operations (default: false)

  // Reverse mapping for O(1) lookup by column index
  csv_header_entry ** index_to_entry; ///< Array mapping column index to header
                                      ///< entry (NULL if no header)
  size_t index_to_entry_capacity;     ///< Capacity of index_to_entry array
};

/**
 * @brief CSV parser state machine states
 */
typedef enum {
  CSV_STATE_START_OF_RECORD,  ///< Start of a new record
  CSV_STATE_START_OF_FIELD,   ///< Start of a new field
  CSV_STATE_UNQUOTED_FIELD,   ///< Accumulating unquoted field
  CSV_STATE_QUOTED_FIELD,     ///< Accumulating quoted field
  CSV_STATE_QUOTE_IN_QUOTED,  ///< Quote character encountered in quoted field
                              ///< (may be escape)
  CSV_STATE_ESCAPE_IN_QUOTED, ///< Backslash encountered in quoted field
                              ///< (backslash-escape mode)
  CSV_STATE_COMMENT,          ///< Processing comment line
  CSV_STATE_END               ///< Parsing complete
} csv_parser_state;

/**
 * @brief Field data structure for accumulating field content
 */
typedef struct {
  const char * start; ///< Start of field data (pointer into input or buffer)
  size_t length;      ///< Length of field data
  bool is_quoted;     ///< Whether field was quoted
  bool needs_copy;    ///< Whether field needs to be copied (for
                      ///< escaping/unescaping)
} csv_field_data;

/**
 * @brief CSV parser structure (internal)
 */
typedef struct {
  // Configuration
  const GTEXT_CSV_Dialect * dialect;
  const GTEXT_CSV_Parse_Options * opts;

  // Input tracking
  const char * input;          ///< Current input buffer
  size_t input_len;            ///< Length of current input buffer
  size_t input_offset;         ///< Offset into current input buffer
  size_t total_bytes_consumed; ///< Total bytes consumed across all feeds

  // Position tracking
  csv_position pos; ///< Current position (byte offset, line, column)

  // State machine
  csv_parser_state state; ///< Current parser state
  bool in_record;         ///< Whether we're currently in a record
  size_t field_count;     ///< Number of fields in current record

  // Field accumulation
  csv_field_data current_field; ///< Current field being accumulated
  char *
      field_buffer; ///< Buffer for field data (when escaping/unescaping needed)
  size_t field_buffer_size; ///< Allocated size of field buffer
  size_t field_buffer_used; ///< Used size of field buffer

  // Limits tracking
  size_t row_count;            ///< Number of rows processed
  size_t max_rows;             ///< Effective max rows limit
  size_t max_cols;             ///< Effective max cols limit
  size_t max_field_bytes;      ///< Effective max field bytes limit
  size_t max_record_bytes;     ///< Effective max record bytes limit
  size_t max_total_bytes;      ///< Effective max total bytes limit
  size_t current_record_bytes; ///< Bytes in current record

  // Error reporting
  GTEXT_CSV_Error * error_out; ///< Error output structure (if provided)

  // Comment handling
  bool in_comment;           ///< Whether we're currently in a comment line
  size_t comment_prefix_len; ///< Length of comment prefix
} csv_parser;

/**
 * @brief CSV writer state enumeration
 */
typedef enum {
  CSV_WRITER_STATE_INITIAL,   ///< Initial state (no record open)
  CSV_WRITER_STATE_IN_RECORD, ///< Record is open (fields can be written)
  CSV_WRITER_STATE_FINISHED   ///< Writer has been finished (no more writes)
} csv_writer_state;

/**
 * @brief CSV writer structure
 */
struct GTEXT_CSV_Writer {
  GTEXT_CSV_Sink sink;          ///< Output sink (not owned)
  GTEXT_CSV_Write_Options opts; ///< Write options (copy)
  csv_writer_state state;       ///< Current writer state
  bool has_fields_in_record;    ///< Whether current record has any fields
  GTEXT_CSV_Status last_error;  ///< Last error status (if any)

  /** Empty fields seen since the last non-empty one, not yet written.
   *
   * ::GTEXT_CSV_Write_Options::trim_trailing_empty_fields drops the empty
   * fields at the *end* of a record, and whether an empty field is at the end
   * is not known until the next field arrives or the record closes.  So an
   * empty field is counted here instead of written: a following non-empty
   * field flushes the run, and gtext_csv_writer_record_end() discards it.
   * Only a count is needed, never the bytes, because an empty field has none.
   * Zero when the option is off, where every field is written as it comes. */
  size_t pending_empty_fields;

  /** Whether a record has been closed whose newline is not written yet.
   *
   * A newline separates records, so every record but the last gets one, and
   * ::GTEXT_CSV_Write_Options::trailing_newline decides the last - which means
   * the writer cannot know at record_end() whether to emit it, because "last"
   * is only known at gtext_csv_writer_finish().  It is emitted when the next
   * record begins, or at finish() if the option asks for it.  This is what
   * makes the two writers agree: the table writer has the whole table and can
   * ask `is_last_row` directly. */
  bool pending_newline;
};

/**
 * @brief Parse context for table building
 */
typedef struct {
  GTEXT_CSV_Table * table;
  csv_table_row * current_row;
  size_t current_field_index;
  size_t current_field_capacity;
  const GTEXT_CSV_Parse_Options * opts;
  GTEXT_CSV_Error * err;
  GTEXT_CSV_Status status;
} csv_table_parse_context;

/**
 * @brief Structure to hold header map pre-allocated structures
 */
typedef struct {
  csv_header_entry ** new_header_map; ///< New header map array
  csv_header_entry ** new_entry_ptrs; ///< Temporary array of entry pointers
  char ** new_name_ptrs;              ///< Temporary array of name pointers
  size_t total_header_entries;        ///< Total number of header entries
} csv_compact_header_map;

/**
 * @brief Structure to hold pre-allocated clone structures
 */
typedef struct {
  csv_context * new_ctx;               ///< New context with arena
  GTEXT_CSV_Table * new_table;         ///< New table structure
  csv_table_row * new_rows;            ///< New rows array
  csv_table_field ** new_field_arrays; ///< Array of field array pointers
  char *** new_field_data_ptrs;        ///< Array of field data pointer arrays
  /// The allocator every array above came from. Carried in the structure
  /// so the cleanup paths reach it without a signature change, and so an
  /// unwind that runs before the context exists still frees correctly.
  const GTEXT_Allocator * alloc;
} csv_clone_structures;

/**
 * @brief Structure to hold pre-allocated header map structures
 */
typedef struct {
  csv_header_entry ** new_header_map; ///< New header map array
  csv_header_entry **
      new_entry_ptrs;          ///< Temporary array to store all new entries
  char ** new_name_ptrs;       ///< Temporary array to store all name strings
  size_t total_header_entries; ///< Total number of header entries
} csv_clone_header_map;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSV_CSV_INTERNAL_H
