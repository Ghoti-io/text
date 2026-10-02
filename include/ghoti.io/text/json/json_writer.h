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
 * JSON writer infrastructure and sink abstraction.
 *
 * This header provides the sink abstraction for writing JSON output to
 * various destinations (buffers, files, callbacks, etc.) and helper
 * functions for common sink types.
 */

#ifndef GHOTI_IO_GTEXT_JSON_JSON_WRITER_H
#define GHOTI_IO_GTEXT_JSON_JSON_WRITER_H

#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_dom.h>
#include <ghoti.io/text/macros.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write callback function type
 *
 * This callback is invoked by the writer to output JSON data chunks.
 * The callback should write the provided bytes to the destination and
 * return 0 on success, or a non-zero value on error.
 *
 * @param user User-provided context pointer
 * @param bytes Pointer to the data to write
 * @param len Number of bytes to write
 * @return 0 on success, non-zero on error
 */
typedef int (*GTEXT_JSON_Write_Function)(
    void * user, const char * bytes, size_t len);

/**
 * @brief JSON output sink structure
 *
 * A sink encapsulates a write callback and user context for outputting
 * JSON data. The writer uses this abstraction to write to various
 * destinations (buffers, files, streams, etc.).
 */
typedef struct {
  GTEXT_JSON_Write_Function write; ///< Write callback function
  void * user;                     ///< User context pointer passed to callback
} GTEXT_JSON_Sink;

/**
 * @brief Growable buffer sink structure
 *
 * Internal structure for managing a growable buffer sink. Users should
 * use GTEXT_JSON_Sink_buffer() to create a sink, not this structure directly.
 */
typedef struct {
  char * data; ///< Buffer data (owned by sink)
  size_t size; ///< Current buffer size (bytes allocated)
  size_t used; ///< Bytes used in buffer
} GTEXT_JSON_Buffer_Sink;

/**
 * @brief Fixed buffer sink structure
 *
 * Internal structure for managing a fixed-size buffer sink. Users should
 * use GTEXT_JSON_Sink_fixed_buffer() to create a sink, not this structure
 * directly.
 */
typedef struct {
  char * data;    ///< Buffer data (provided by user, not owned)
  size_t size;    ///< Maximum buffer size
  size_t used;    ///< Bytes written to buffer
  bool truncated; ///< true if truncation occurred
} GTEXT_JSON_Fixed_Buffer_Sink;

/**
 * @brief Create a growable buffer sink
 *
 * Creates a sink that writes to a dynamically-growing buffer. The buffer
 * is allocated and managed by the sink. Use GTEXT_JSON_Sink_buffer_data()
 * and GTEXT_JSON_Sink_buffer_size() to access the buffer, and
 * GTEXT_JSON_Sink_buffer_free() to free it.
 *
 * @param sink Output parameter for the created sink
 * @return GTEXT_JSON_OK on success, GTEXT_JSON_E_OOM on allocation failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_sink_buffer(GTEXT_JSON_Sink * sink);

/**
 * @brief Get the buffer data from a growable buffer sink
 *
 * Returns a pointer to the buffer data. The buffer is null-terminated
 * for convenience, but may contain null bytes in the middle. Use
 * GTEXT_JSON_Sink_buffer_size() to get the actual size.
 *
 * @param sink Sink created by GTEXT_JSON_Sink_buffer()
 * @return Pointer to buffer data, or NULL if sink is invalid
 */
GTEXT_API const char * gtext_json_sink_buffer_data(
    const GTEXT_JSON_Sink * sink);

/**
 * @brief Get the buffer size from a growable buffer sink
 *
 * Returns the number of bytes written to the buffer (not including
 * the null terminator).
 *
 * @param sink Sink created by GTEXT_JSON_Sink_buffer()
 * @return Number of bytes written, or 0 if sink is invalid
 */
GTEXT_API size_t gtext_json_sink_buffer_size(const GTEXT_JSON_Sink * sink);

/**
 * @brief Free a growable buffer sink
 *
 * Frees the buffer allocated by GTEXT_JSON_Sink_buffer(). After calling
 * this function, the sink is invalid and should not be used.
 *
 * @param sink Sink created by GTEXT_JSON_Sink_buffer()
 */
GTEXT_API void gtext_json_sink_buffer_free(GTEXT_JSON_Sink * sink);

/**
 * @brief Create a fixed-size buffer sink
 *
 * Creates a sink that writes to a fixed-size buffer provided by the caller.
 * If the output exceeds the buffer size, it will be truncated and the
 * truncated flag will be set. Use GTEXT_JSON_Sink_fixed_buffer_used() to
 * get the number of bytes written, and GTEXT_JSON_Sink_fixed_buffer_truncated()
 * to check if truncation occurred.
 *
 * @param sink Output parameter for the created sink
 * @param buffer Buffer to write to (must remain valid for sink lifetime)
 * @param size Maximum size of the buffer
 * @return GTEXT_JSON_OK on success, GTEXT_JSON_E_INVALID if buffer is NULL or
 * size is 0
 */
GTEXT_API GTEXT_JSON_Status gtext_json_sink_fixed_buffer(
    GTEXT_JSON_Sink * sink, char * buffer, size_t size);

/**
 * @brief Get the number of bytes written to a fixed buffer sink
 *
 * Returns the number of bytes written to the buffer (may be less than
 * the buffer size if truncation occurred).
 *
 * @param sink Sink created by GTEXT_JSON_Sink_fixed_buffer()
 * @return Number of bytes written, or 0 if sink is invalid
 */
GTEXT_API size_t gtext_json_sink_fixed_buffer_used(
    const GTEXT_JSON_Sink * sink);

/**
 * @brief Check if truncation occurred in a fixed buffer sink
 *
 * Returns true if the output was truncated due to insufficient buffer
 * space, false otherwise.
 *
 * @param sink Sink created by GTEXT_JSON_Sink_fixed_buffer()
 * @return true if truncated, false otherwise
 */
GTEXT_API bool gtext_json_sink_fixed_buffer_truncated(
    const GTEXT_JSON_Sink * sink);

/**
 * @brief Free a fixed buffer sink
 *
 * Frees the internal structure allocated by GTEXT_JSON_Sink_fixed_buffer().
 * After calling this function, the sink is invalid and should not be used.
 * Note: This does NOT free the buffer itself (it's owned by the caller).
 *
 * @param sink Sink created by GTEXT_JSON_Sink_fixed_buffer()
 */
GTEXT_API void gtext_json_sink_fixed_buffer_free(GTEXT_JSON_Sink * sink);

/**
 * @brief Write a JSON value to a sink
 *
 * Serializes a JSON DOM value to JSON text using the provided sink and
 * write options. Supports both compact and pretty-print output modes,
 * configurable escaping, and canonical output options.
 *
 * @param sink Output sink (must not be NULL)
 * @param opt Write options (can be NULL for defaults)
 * @param v JSON value to write (must not be NULL)
 * @param err Error output structure (can be NULL if error details not needed)
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_write_value(GTEXT_JSON_Sink * sink,
    const GTEXT_JSON_Write_Options * opt, const GTEXT_JSON_Value * v,
    GTEXT_JSON_Error * err);

/**
 * @brief Forward declaration of streaming writer structure
 *
 * The streaming writer maintains internal state to enforce structural
 * correctness (e.g., preventing values without keys inside objects).
 */
typedef struct GTEXT_JSON_Writer GTEXT_JSON_Writer;

/**
 * @brief Create a new streaming JSON writer
 *
 * Creates a new streaming writer that writes JSON output to the provided
 * sink using the specified write options. The writer enforces structural
 * correctness (e.g., prevents writing values without keys inside objects).
 *
 * **Four write options cannot apply here, and are ignored rather than
 * refused.** Each needs to see the whole of a value before its first byte is
 * written, which is exactly what this API gives up:
 *
 * - ::GTEXT_JSON_Write_Options::sort_object_keys - the keys are not all known
 *   until the object closes, and the first one is already written by then.
 * - ::GTEXT_JSON_Write_Options::inline_array_threshold and
 *   ::GTEXT_JSON_Write_Options::inline_object_threshold - both decide bracket
 *   style from the element count, which is not known when the bracket is
 *   written.
 * - ::GTEXT_JSON_Write_Options::canonical_numbers - it selects reformatting
 *   from a parsed value *instead of* a lexeme preserved by a parse. Nothing
 *   here was parsed: gtext_json_writer_number_lexeme() is the caller handing
 *   over bytes, and gtext_json_writer_number_double() formats. The choice the
 *   option makes is the choice of which function to call.
 *
 * Everything else behaves as it does in gtext_json_write_value(), byte for
 * byte, and that is checked by a test comparing the two writers over the same
 * document for every remaining option. They did not always agree: an object of
 * two or more members used to come out as `{"a":1,"b":,2}` from this API, and
 * the separator rule is shared with the value writer now.
 *
 * @param sink Output sink (must not be NULL)
 * @param opt Write options (can be NULL for defaults). See above for the four
 *            this API cannot honour.
 * @return New writer instance, or NULL on allocation failure
 */
GTEXT_API GTEXT_JSON_Writer * gtext_json_writer_new(
    GTEXT_JSON_Sink sink, const GTEXT_JSON_Write_Options * opt);

/**
 * @brief Free a streaming JSON writer
 *
 * Frees all resources associated with the writer. After calling this
 * function, the writer pointer is invalid and must not be used.
 *
 * @param w Writer to free (can be NULL, in which case this is a no-op)
 */
GTEXT_API void gtext_json_writer_free(GTEXT_JSON_Writer * w);

/**
 * @brief Begin writing an object
 *
 * Writes the opening brace `{` for a JSON object. Must be followed by
 * key-value pairs or be closed immediately with gtext_json_writer_object_end().
 *
 * @param w Writer instance (must not be NULL)
 * @return GTEXT_JSON_OK on success, error code on failure (e.g., invalid state)
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_object_begin(
    GTEXT_JSON_Writer * w);

/**
 * @brief End writing an object
 *
 * Writes the closing brace `}` for a JSON object. The object must have
 * been started with gtext_json_writer_object_begin().
 *
 * @param w Writer instance (must not be NULL)
 * @return GTEXT_JSON_OK on success, error code on failure (e.g., incomplete
 * object)
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_object_end(GTEXT_JSON_Writer * w);

/**
 * @brief Begin writing an array
 *
 * Writes the opening bracket `[` for a JSON array. Must be followed by
 * values or be closed immediately with gtext_json_writer_array_end().
 *
 * @param w Writer instance (must not be NULL)
 * @return GTEXT_JSON_OK on success, error code on failure (e.g., invalid state)
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_array_begin(
    GTEXT_JSON_Writer * w);

/**
 * @brief End writing an array
 *
 * Writes the closing bracket `]` for a JSON array. The array must have
 * been started with gtext_json_writer_array_begin().
 *
 * @param w Writer instance (must not be NULL)
 * @return GTEXT_JSON_OK on success, error code on failure (e.g., incomplete
 * array)
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_array_end(GTEXT_JSON_Writer * w);

/**
 * @brief Write an object key
 *
 * Writes a key string for an object key-value pair. Must be called inside
 * an object context (after object_begin, before the corresponding value).
 * The key will be properly escaped according to write options.
 *
 * @param w Writer instance (must not be NULL)
 * @param key Key string (must not be NULL)
 * @param len Length of key string in bytes
 * @return GTEXT_JSON_OK on success, error code on failure (e.g., not in object
 * context)
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_key(
    GTEXT_JSON_Writer * w, const char * key, size_t len);

/**
 * @brief Write a null value
 *
 * Writes the JSON null value. Can be used in arrays or as object values.
 *
 * @param w Writer instance (must not be NULL)
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_null(GTEXT_JSON_Writer * w);

/**
 * @brief Write a boolean value
 *
 * Writes a JSON boolean value (true or false).
 *
 * @param w Writer instance (must not be NULL)
 * @param b Boolean value
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_bool(
    GTEXT_JSON_Writer * w, bool b);

/**
 * @brief Write a number value from lexeme
 *
 * Writes a JSON number value using the exact lexeme string. The lexeme
 * should be a valid JSON number format.
 *
 * @param w Writer instance (must not be NULL)
 * @param s Number lexeme string (must not be NULL)
 * @param len Length of lexeme string in bytes
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_number_lexeme(
    GTEXT_JSON_Writer * w, const char * s, size_t len);

/**
 * @brief Write a number value from int64
 *
 * Writes a JSON number value formatted from a signed 64-bit integer.
 *
 * @param w Writer instance (must not be NULL)
 * @param x Integer value to write
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_number_i64(
    GTEXT_JSON_Writer * w, long long x);

/**
 * @brief Write a number value from uint64
 *
 * Writes a JSON number value formatted from an unsigned 64-bit integer.
 *
 * @param w Writer instance (must not be NULL)
 * @param x Unsigned integer value to write
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_number_u64(
    GTEXT_JSON_Writer * w, unsigned long long x);

/**
 * @brief Write a number value from double
 *
 * Writes a JSON number value formatted from a double-precision floating-point
 * number. Non-finite numbers (NaN, Infinity) are only written if the
 * allow_nonfinite_numbers option is enabled.
 *
 * @param w Writer instance (must not be NULL)
 * @param x Floating-point value to write
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_number_double(
    GTEXT_JSON_Writer * w, double x);

/**
 * @brief Write a string value
 *
 * Writes a JSON string value. The string will be properly escaped according
 * to write options (escape sequences, Unicode escaping, etc.).
 *
 * @param w Writer instance (must not be NULL)
 * @param s String data (must not be NULL)
 * @param len Length of string in bytes
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_string(
    GTEXT_JSON_Writer * w, const char * s, size_t len);

/**
 * @brief Finish writing and validate structure
 *
 * Completes the JSON output and validates that the structure is complete
 * (all objects and arrays are properly closed). Returns an error if the
 * structure is incomplete or invalid.
 *
 * @param w Writer instance (must not be NULL)
 * @param err Error output structure (can be NULL if error details not needed)
 * @return GTEXT_JSON_OK on success, error code on failure (e.g., incomplete
 * structure)
 */
GTEXT_API GTEXT_JSON_Status gtext_json_writer_finish(
    GTEXT_JSON_Writer * w, GTEXT_JSON_Error * err);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_JSON_JSON_WRITER_H
