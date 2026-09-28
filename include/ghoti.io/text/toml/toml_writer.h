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
 * @file toml_writer.h
 * @brief Writing a TOML document, and the sinks it writes through.
 *
 * The write options live here rather than in toml_core.h, where the other
 * formats in this library keep theirs. They hold a `GCHRON_WriteOptions *`,
 * and toml_core.h is deliberately the one header of this module that names no
 * date-time type: a caller who only wants the status codes should not acquire
 * `chron` along with them.
 *
 * What the writer chooses, and why, is in @ref format_toml under Save. The
 * short version: a table is written the way it was read - `[header]` for a
 * table a header defined, `{ }` for one written inline - and
 * @ref GTEXT_TOML_Write_Options::table_style overrides that in either
 * direction.
 */

#ifndef GHOTI_IO_GTEXT_TOML_TOML_WRITER_H
#define GHOTI_IO_GTEXT_TOML_TOML_WRITER_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/toml/toml_core.h>
#include <ghoti.io/text/toml/toml_dom.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A sink's write callback.
 *
 * Returns GTEXT_TOML_OK, or a status the writer passes back to its caller
 * unchanged. Returning GTEXT_TOML_E_WRITE is the usual way to say "the
 * destination refused"; returning GTEXT_TOML_E_OOM keeps an allocation failure
 * inside a sink distinguishable from one, which is the distinction a caller
 * retrying on a full buffer needs and which an `int` return throws away.
 *
 * @param user The sink's user pointer.
 * @param bytes The bytes to write. Never NULL; may contain a NUL, because a
 *   TOML string may.
 * @param len How many bytes. Never 0.
 */
typedef GTEXT_TOML_Status (*GTEXT_TOML_Write_Function)(
    void * user, const char * bytes, size_t len);

/**
 * @struct GTEXT_TOML_Sink
 * @brief Where written bytes go.
 */
typedef struct {
  GTEXT_TOML_Write_Function write; ///< Called for each chunk.
  void * user;                     ///< Passed back unchanged.
} GTEXT_TOML_Sink;

/**
 * @brief Point @p sink at a fresh growable buffer.
 *
 * The buffer comes from the default allocator, not from a write option's, for
 * the reason gtext_toml_error_free() gives about the error snippet: a sink is
 * created before any options are seen and outlives the write, so the pair that
 * allocates and frees it has to be fixed rather than following an option.
 *
 * @param sink Receives the sink. Must not be NULL.
 * @return GTEXT_TOML_OK, GTEXT_TOML_E_INVALID or GTEXT_TOML_E_OOM.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_sink_buffer(GTEXT_TOML_Sink * sink);

/**
 * @brief The bytes a growable buffer sink has collected.
 *
 * NUL-terminated for convenience; gtext_toml_sink_buffer_size() is
 * authoritative, since TOML permits a NUL inside a string and therefore inside
 * a written document.
 *
 * @param sink A sink from gtext_toml_sink_buffer().
 * @return The bytes, or NULL if @p sink is not a growable buffer sink.
 */
GTEXT_API const char * gtext_toml_sink_buffer_data(
    const GTEXT_TOML_Sink * sink);

/**
 * @brief How many bytes a growable buffer sink holds.
 *
 * @param sink A sink from gtext_toml_sink_buffer().
 * @return The count, or 0.
 */
GTEXT_API size_t gtext_toml_sink_buffer_size(const GTEXT_TOML_Sink * sink);

/**
 * @brief Release a growable buffer sink.
 *
 * Safe on a zeroed struct and twice. Leaves @p sink zeroed.
 *
 * @param sink The sink. NULL is ignored.
 */
GTEXT_API void gtext_toml_sink_buffer_free(GTEXT_TOML_Sink * sink);

/**
 * @brief Point @p sink at a caller-owned fixed buffer.
 *
 * Writing more than fits fills the buffer, sets the truncated flag, and makes
 * every later write return GTEXT_TOML_E_WRITE - so a truncated document is
 * reported as a failure rather than handed back as a shorter one. A caller who
 * wants to size a buffer should write to a growable sink and read the size, or
 * check gtext_toml_sink_fixed_buffer_truncated() rather than the byte count,
 * because a full buffer and an exactly-fitting document have the same count.
 *
 * @param sink Receives the sink. Must not be NULL.
 * @param buffer Where to write. Must not be NULL.
 * @param size Bytes available at @p buffer. Must not be 0.
 * @return GTEXT_TOML_OK, GTEXT_TOML_E_INVALID or GTEXT_TOML_E_OOM.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_sink_fixed_buffer(
    GTEXT_TOML_Sink * sink, char * buffer, size_t size);

/**
 * @brief How many bytes a fixed buffer sink has taken.
 *
 * @param sink A sink from gtext_toml_sink_fixed_buffer().
 * @return The count, or 0.
 */
GTEXT_API size_t gtext_toml_sink_fixed_buffer_used(
    const GTEXT_TOML_Sink * sink);

/**
 * @brief Whether a fixed buffer sink ran out of room.
 *
 * @param sink A sink from gtext_toml_sink_fixed_buffer().
 * @return true if anything was dropped.
 */
GTEXT_API bool gtext_toml_sink_fixed_buffer_truncated(
    const GTEXT_TOML_Sink * sink);

/**
 * @brief Release a fixed buffer sink's bookkeeping.
 *
 * The caller's buffer is not touched. Safe twice; leaves @p sink zeroed.
 *
 * @param sink The sink. NULL is ignored.
 */
GTEXT_API void gtext_toml_sink_fixed_buffer_free(GTEXT_TOML_Sink * sink);

/**
 * @enum GTEXT_TOML_Table_Style
 * @brief How the writer spells a table.
 *
 * TOML can spell one value two ways, and neither is canonical: `[a]` with
 * `b = 1` under it, and `a = { b = 1 }`, are the same document. The reader
 * records which was used, so the default here reproduces it; the other two
 * settings are for a caller who wants one shape whatever arrived.
 */
typedef enum {
  /**
   * Write each table the way it was read, and each array the way it was read:
   * a table a `[header]` defined gets a header, one written `{ }` stays
   * inline, and an array grown by `[[header]]` gets headers where an array
   * written `[ ]` stays inline even if every element is a table.
   *
   * A table built through gtext_toml_new_table() counts as a header table and
   * an array through gtext_toml_new_array() as a static array, which is what
   * gtext_toml_value_set_inline() is for.
   */
  GTEXT_TOML_TABLE_STYLE_AS_READ = 0,
  /**
   * Every table gets a `[header]` and every array whose elements are all
   * tables gets `[[header]]`, whatever it was read as.
   *
   * An *empty* array still has to be written `key = []`: zero `[[key]]`
   * headers would not say that the key exists at all.
   */
  GTEXT_TOML_TABLE_STYLE_HEADERS = 1,
  /**
   * No headers at all: the document is bare `key = value` lines whose values
   * are inline tables and arrays.
   *
   * TOML 1.0.0 forbids a newline inside `{ }`, so a deep document becomes one
   * very long line. It is still a valid document, and it is the shape with no
   * ordering rule to get wrong.
   */
  GTEXT_TOML_TABLE_STYLE_INLINE = 2
} GTEXT_TOML_Table_Style;

/**
 * @struct GTEXT_TOML_Write_Options
 * @brief What a write is allowed to do, and how it spells things.
 *
 * Zeroed is the default, unlike @ref GTEXT_TOML_Parse_Options - there is no
 * limit here whose zero means "unlimited". Still prefer
 * gtext_toml_write_options_default(), so that a field added later arrives with
 * its intended value rather than with zero.
 */
typedef struct {
  /**
   * The allocator the write's own scratch goes through, or NULL for
   * gtext_allocator_default(). The document is not copied, so this is the
   * frame stack and the header-path buffer and nothing else.
   */
  const GTEXT_Allocator * allocator;

  /** How to spell tables. Default GTEXT_TOML_TABLE_STYLE_AS_READ. */
  GTEXT_TOML_Table_Style table_style;

  /**
   * How to spell date-times, or NULL for `chron`'s defaults - uppercase `T`,
   * `Z` for a known zero offset, and exactly the fraction the value carries.
   *
   * `GCHRON_WriteOptions::space_separator` produces the space-separated form
   * TOML permits (v1.0.0, Offset Date-Time: "a space may be used to separate
   * the date and time"). An unknown offset is written `-00:00` whatever is
   * asked for, because that spelling is the only one carrying the meaning.
   */
  const GCHRON_WriteOptions * datetime;
} GTEXT_TOML_Write_Options;

/**
 * @brief The default write options: default allocator, tables as they were
 *   read, `chron`'s date-time defaults.
 *
 * @return The defaults, by value.
 */
GTEXT_API GTEXT_TOML_Write_Options gtext_toml_write_options_default(void);

/**
 * @brief Write a document to a sink.
 *
 * The walk is on the heap, not the C stack, so a document as deep as the
 * parser accepted can always be written - the same contract
 * gtext_toml_free() keeps, and for the same reason: a writer that recursed
 * would crash on a tree this library handed the caller itself.
 *
 * Output ends with a newline, and is empty for an empty root table. Inside one
 * table, keys keep the order they were defined in, except that sub-tables
 * written as headers come after the plain keys whatever order they were
 * defined in - TOML has no choice about that, since every bare key after a
 * `[header]` belongs to the table that header opened.
 *
 * The writer refuses rather than emitting a document it could not read back:
 * a string or key that is not valid UTF-8 is GTEXT_TOML_E_BAD_UNICODE, and a
 * date-time `chron` will not spell is GTEXT_TOML_E_DATETIME. Output written
 * before the refusal has already reached the sink.
 *
 * @param root The document. Must be a table - v1.0.0 has no other kind of
 *   document - and anything else is GTEXT_TOML_E_INVALID.
 * @param sink Where to write. Must not be NULL.
 * @param opts Options, or NULL for gtext_toml_write_options_default().
 * @return GTEXT_TOML_OK, or the first failure.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_write(const GTEXT_TOML_Value * root,
    GTEXT_TOML_Sink * sink, const GTEXT_TOML_Write_Options * opts);

/**
 * @brief Write a document to a file, atomically.
 *
 * The bytes go to a temporary file beside the destination and replace it only
 * once they are all committed, so a failure part way through leaves the
 * previous file whole rather than truncated. Configuration files are what this
 * parser is pointed at, and a half-written one is worse than an old one.
 *
 * @param root The document.
 * @param path Where to write.
 * @param opts Options, or NULL for the defaults.
 * @return GTEXT_TOML_OK, GTEXT_TOML_E_WRITE, or a failure from the write
 *   itself.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_write_file(const GTEXT_TOML_Value * root,
    const char * path, const GTEXT_TOML_Write_Options * opts);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_TOML_TOML_WRITER_H
