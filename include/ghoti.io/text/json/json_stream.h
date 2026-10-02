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
 * Streaming (incremental) JSON parser API.
 *
 * This header provides an event-based streaming parser that accepts input
 * in chunks and emits events for each JSON value encountered. This is useful
 * for parsing large JSON documents without building a full DOM tree in memory.
 */

#ifndef GHOTI_IO_GTEXT_JSON_JSON_STREAM_H
#define GHOTI_IO_GTEXT_JSON_JSON_STREAM_H

#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/macros.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event types emitted by the streaming parser
 */
typedef enum {
  GTEXT_JSON_EVT_NULL,         ///< null value
  GTEXT_JSON_EVT_BOOL,         ///< boolean value (true/false)
  GTEXT_JSON_EVT_NUMBER,       ///< number value (lexeme always available)
  GTEXT_JSON_EVT_STRING,       ///< string value (decoded UTF-8)
  GTEXT_JSON_EVT_ARRAY_BEGIN,  ///< Array start marker
  GTEXT_JSON_EVT_ARRAY_END,    ///< Array end marker
  GTEXT_JSON_EVT_OBJECT_BEGIN, ///< Object start marker
  GTEXT_JSON_EVT_OBJECT_END,   ///< Object end marker
  GTEXT_JSON_EVT_KEY,          ///< Object key (before value)

  /**
   * One top-level value is complete.
   *
   * Emitted only when ::GTEXT_JSON_Parse_Options::records is not
   * ::GTEXT_JSON_RECORDS_OFF, once per record, after that record's last event
   * and before anything belonging to the next one. The last record's
   * GTEXT_JSON_EVT_RECORD_END arrives from gtext_json_stream_finish(), because
   * until then the input may still be a longer value.
   *
   * It carries no data. It exists because the events alone cannot say where a
   * record ended: `1 2` is two records and emits two GTEXT_JSON_EVT_NUMBER
   * events, which is also what the single value `[1,2]` emits between its
   * array markers, and a consumer building one object per record needs to know
   * which. Appended to this enumeration rather than grouped with the other
   * structural markers so that no existing enumerator's value moves.
   */
  GTEXT_JSON_EVT_RECORD_END
} GTEXT_JSON_Event_Type;

/**
 * @brief Event structure emitted by the streaming parser
 *
 * Contains the event type and associated data. String and number data
 * are valid only for the duration of the callback invocation.
 */
typedef struct {
  GTEXT_JSON_Event_Type type; ///< Event type
  union {
    bool boolean; ///< For GTEXT_JSON_EVT_BOOL
    struct {
      const char * s; ///< String data (decoded UTF-8, null-terminated)
      size_t len;     ///< String length in bytes
    } str;            ///< For GTEXT_JSON_EVT_STRING and GTEXT_JSON_EVT_KEY
    struct {
      const char * s; ///< Number lexeme (exact token text)
      size_t len;     ///< Lexeme length in bytes
    } number;         ///< For GTEXT_JSON_EVT_NUMBER
  } as;

  /**
   * @brief This name has already appeared in the enclosing object.
   *
   * Meaningful on ::GTEXT_JSON_EVT_KEY and `false` on every other event.
   *
   * ## Why this exists rather than the parser applying the policy
   *
   * ::GTEXT_JSON_Parse_Options::dupkeys has four values, and a streaming
   * parser can only honour two of them itself. ::GTEXT_JSON_DUPKEY_ERROR
   * refuses the document, and ::GTEXT_JSON_DUPKEY_FIRST_WINS parses the
   * repeated member and delivers none of its events - both are decisions it
   * can take before the callback has seen anything.
   *
   * ::GTEXT_JSON_DUPKEY_LAST_WINS and ::GTEXT_JSON_DUPKEY_COLLECT are not like
   * that. Last-wins means replacing a value the callback has already been
   * handed, and collect means wrapping it in an array after the fact; an event
   * cannot be retracted, and buffering until the object closes is not
   * streaming. So under those two modes **every member is delivered**, which
   * read as a silent disagreement with gtext_json_parse() on the same option
   * and was recorded as an adoption blocker on the comparison page.
   *
   * This is the information the caller needs to apply either policy itself,
   * and it is the only part the parser is in a position to supply: whichever
   * structure the callback is building, last-wins is "overwrite what you
   * stored under this name" and collect is "append to it", and both are one
   * line at the point where it stores a member.
   *
   * It is never `true` under ::GTEXT_JSON_DUPKEY_ERROR or
   * ::GTEXT_JSON_DUPKEY_FIRST_WINS, because neither of those delivers a key
   * event for a repeated name at all.
   *
   * Names are compared as bytes after decoding, so `"a"` and `"\u0061"` are
   * the same name; with ::GTEXT_JSON_Parse_Options::normalize_unicode they are
   * compared after normalisation, exactly as gtext_json_parse() compares them.
   *
   * Appended after the union rather than placed beside `type`, so that the
   * offset of every field that was already here stays where it was. The
   * struct's *size* does grow, and gtext_json_reader_next() takes one by
   * pointer from the caller, so a caller has to be recompiled against this
   * header - which is true of every options struct in this library for the
   * same reason.
   */
  bool repeated_key;
} GTEXT_JSON_Event;

/**
 * @brief Event callback function type
 *
 * Called by the streaming parser for each event encountered. The callback
 * should return GTEXT_JSON_OK to continue parsing, or a non-zero error code
 * to stop parsing.
 *
 * @param user User-provided context pointer
 * @param evt Event structure (valid only during callback)
 * @param err Error structure (can be populated by callback to report errors)
 * @return GTEXT_JSON_OK to continue, non-zero to stop parsing
 */
typedef GTEXT_JSON_Status (*GTEXT_JSON_Event_cb)(
    void * user, const GTEXT_JSON_Event * evt, GTEXT_JSON_Error * err);

/**
 * @brief Forward declaration of streaming parser structure
 *
 * The actual structure is defined internally. Streams are created via
 * gtext_json_stream_new() and freed via gtext_json_stream_free().
 */
typedef struct GTEXT_JSON_Stream GTEXT_JSON_Stream;

/**
 * @brief Create a new streaming parser
 *
 * Creates a new streaming parser instance with the specified parse options
 * and event callback. The parser accepts input via gtext_json_stream_feed()
 * and emits events through the callback.
 *
 * **Parameter Validation:**
 * - If `cb` is NULL, returns NULL (callback is required)
 * - If `opt` is NULL, uses default parse options
 *
 * **Error Handling:**
 * - Returns NULL on allocation failure
 * - All resources are cleaned up automatically on failure
 *
 * **Resource Cleanup:**
 * - Caller must free returned stream using gtext_json_stream_free()
 * - Stream must be freed even if feed/finish operations fail
 *
 * @param opt Parse options (can be NULL for defaults)
 * @param cb Event callback function (must not be NULL)
 * @param user User context pointer passed to callback
 * @return New stream instance, or NULL on allocation failure
 */
GTEXT_API GTEXT_JSON_Stream * gtext_json_stream_new(
    const GTEXT_JSON_Parse_Options * opt, GTEXT_JSON_Event_cb cb, void * user);

/**
 * @brief Feed input data to the streaming parser
 *
 * Processes the provided input chunk and emits events through the callback.
 * The parser maintains state between calls, allowing incremental parsing
 * of large inputs.
 *
 * **Multi-Chunk Value Handling:**
 * The parser correctly handles values (strings, numbers) that span multiple
 * chunks. When a value is incomplete at the end of a chunk, the parser
 * preserves state and waits for more input. Values can span an unlimited
 * number of chunks, limited only by the `max_total_bytes` option (default:
 * 64MB).
 *
 * **Examples:**
 * - String spanning chunks: `"hello` (chunk 1) + `world"` (chunk 2) ->
 * `"helloworld"`
 * - Number spanning chunks: `12345` (chunk 1) + `.678` (chunk 2) -> `12345.678`
 * - Values can span 2, 3, 100, or more chunks as long as total size is within
 * limits
 *
 * **Important:** If the last value in the JSON is incomplete at the end of
 * the final chunk, it will not be emitted until `gtext_json_stream_finish()`
 * is called. Always call `finish()` after feeding all input to ensure all
 * values are processed and emitted.
 *
 * **Parameter Validation:**
 * - If `st` is NULL, returns GTEXT_JSON_E_INVALID
 * - If `bytes` is NULL, returns GTEXT_JSON_E_INVALID
 * - If `len` exceeds SIZE_MAX/2, returns GTEXT_JSON_E_INVALID
 *   (prevents obvious overflow in internal calculations)
 * - If `err` is NULL, error details are not populated
 * - State validation ensures stream is in a valid state for feeding
 *
 * **Overflow Protection:**
 * - All arithmetic operations are protected against integer overflow
 * - Input size validation prevents overflow in buffer calculations
 * - String length, container size, and total bytes are validated against limits
 * - Buffer growth operations use overflow-safe calculations
 *
 * **Error Handling:**
 * - Returns error code on failure (parse error, limit exceeded, state error)
 * - Error details are populated in `err` structure if provided
 * - Error structure includes position information (offset, line, column)
 * - Context snippets are generated for better error diagnostics
 * - Stream enters error state on failure (subsequent operations return error)
 *
 * **Resource Cleanup:**
 * - On error: stream state is preserved for error reporting
 * - Buffered data is maintained until stream is freed
 * - Error context snippets (if generated) must be freed via
 * GTEXT_JSON_Error_free()
 *
 * @param st Stream instance (must not be NULL)
 * @param bytes Input data (must not be NULL)
 * @param len Length of input data in bytes
 * @param err Error structure for error reporting (can be NULL)
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_stream_feed(GTEXT_JSON_Stream * st,
    const char * bytes, size_t len, GTEXT_JSON_Error * err);

/**
 * @brief Finish parsing and validate structure
 *
 * Signals that no more input will be provided. This function:
 * - Processes any remaining buffered input (including incomplete values)
 * - Emits any final events that were waiting for completion
 * - Validates that the JSON structure is complete (no unmatched brackets, etc.)
 *
 * **Important:** Always call this function after feeding all input chunks.
 * The last value may not be emitted until `finish()` is called, especially
 * if it was incomplete at the end of the final chunk (e.g., a number ending
 * with a digit, or a string without a closing quote).
 *
 * **Parameter Validation:**
 * - If `st` is NULL, returns GTEXT_JSON_E_INVALID
 * - If `err` is NULL, error details are not populated
 * - State validation ensures stream is in a valid state for finishing
 *
 * **Error Handling:**
 * - Returns error code on failure (incomplete structure, parse error)
 * - Error details are populated in `err` structure if provided
 * - Error structure includes position information (offset, line, column)
 * - Stream enters error state on failure
 *
 * **Resource Cleanup:**
 * - On success: stream is ready for freeing (no more operations needed)
 * - On error: stream state is preserved for error reporting
 * - Error context snippets (if generated) must be freed via
 * GTEXT_JSON_Error_free()
 *
 * @param st Stream instance (must not be NULL)
 * @param err Error structure for error reporting (can be NULL)
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_stream_finish(
    GTEXT_JSON_Stream * st, GTEXT_JSON_Error * err);

/**
 * @brief Free a streaming parser instance
 *
 * Frees all resources associated with the stream, including any buffered
 * data. After calling this function, the stream pointer is invalid.
 *
 * **Parameter Validation:**
 * - If `st` is NULL, this function is a no-op (safe to call with NULL)
 *
 * **Resource Cleanup:**
 * - Frees all internal buffers (input buffer, token buffer, stack)
 * - Frees any error context snippets that were allocated
 * - All resources are properly cleaned up (no memory leaks)
 * - Safe to call even if stream is in an error state
 *
 * @param st Stream instance to free (can be NULL, in which case this is a
 * no-op)
 */
GTEXT_API void gtext_json_stream_free(GTEXT_JSON_Stream * st);

/**
 * @brief Opaque pull-model reader structure
 */
typedef struct GTEXT_JSON_Reader GTEXT_JSON_Reader;

/**
 * @brief Create a new pull-model JSON reader
 *
 * The streaming parser above calls the caller; this lets the caller call the
 * parser. It wraps the streaming parser and queues events for
 * gtext_json_reader_next(), copying each event's bytes into that queue - which
 * it has to, because the events this header describes are "valid only for the
 * duration of the callback invocation".
 *
 * The same four calls as the YAML and CSV readers, in the same order.
 *
 * @param opts Parse options, or NULL for defaults.
 *             GTEXT_JSON_Parse_Options::allocator covers the reader, its queue
 *             and the copied bytes.
 * @return New reader, or NULL on allocation failure
 */
GTEXT_API GTEXT_JSON_Reader * gtext_json_reader_new(
    const GTEXT_JSON_Parse_Options * opts);

/**
 * @brief Feed input to the pull reader
 *
 * To signal end of input, call with `data` NULL and `len` 0, which finishes the
 * parse and enqueues any remaining events. Doing that twice is accepted and does
 * nothing. Feeding bytes after end of input is GTEXT_JSON_E_STATE.
 *
 * @param reader Reader (must not be NULL)
 * @param data Input chunk, or NULL with len 0 for end of input
 * @param len Length of the chunk
 * @param err Error output, or NULL
 * @return GTEXT_JSON_OK, or the parse error
 */
GTEXT_API GTEXT_JSON_Status gtext_json_reader_feed(GTEXT_JSON_Reader * reader,
    const void * data, size_t len, GTEXT_JSON_Error * err);

/**
 * @brief Take the next available event from the reader
 *
 * @param reader Reader (must not be NULL)
 * @param out_event Receives the event (must not be NULL)
 * @return GTEXT_JSON_OK when an event was available;
 *         GTEXT_JSON_E_INCOMPLETE when more input is needed;
 *         GTEXT_JSON_E_STATE when input has ended and the queue is empty;
 *         or the parse error, repeated on every later call once one has
 *         happened, so a failure is never mistaken for the end.
 *
 * The event's string and number pointers stay valid until the next call to
 * gtext_json_reader_next() or gtext_json_reader_free(), whichever comes first -
 * which is longer than the push callback's events live, and is the reason this
 * reader copies.
 */
GTEXT_API GTEXT_JSON_Status gtext_json_reader_next(
    GTEXT_JSON_Reader * reader, GTEXT_JSON_Event * out_event);

/**
 * @brief Free the reader, its queue and any event still held
 *
 * Passing NULL is a no-op.
 */
GTEXT_API void gtext_json_reader_free(GTEXT_JSON_Reader * reader);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_JSON_JSON_STREAM_H
