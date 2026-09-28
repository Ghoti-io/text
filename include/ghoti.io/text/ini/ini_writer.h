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
 * @file ini_writer.h
 * @brief Writing an INI document back out.
 *
 * **A document that was parsed and not modified writes back byte for byte.**
 * That is a requirement rather than a nicety: Desktop Entry §3 says a
 * compliant implementation "MUST not remove any fields from the file, even if
 * they don't support them", that such fields "must be maintained in a list
 * somewhere, and if the file is 'rewritten', they will be included", and that
 * comments "should be preserved across reads and writes". So the writer emits
 * each entry's original line where it has one, and synthesizes `key=value` only
 * for an entry a caller created or changed.
 *
 * The consequence to know about: this writer does not normalize. A document
 * that came in with `k  =  v` goes out that way. A caller who wants normalized
 * output sets ::GTEXT_INI_Write_Options::normalize, and then the round trip is
 * no longer byte-identical by design.
 */

#ifndef GHOTI_IO_GTEXT_INI_INI_WRITER_H
#define GHOTI_IO_GTEXT_INI_INI_WRITER_H

#include <ghoti.io/text/ini/ini_dom.h>
#include <ghoti.io/text/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A sink's write callback.
 *
 * @param user The sink's user pointer.
 * @param bytes The bytes to write. Never NULL; may contain a NUL.
 * @param len How many bytes. Never 0.
 * @return ::GTEXT_INI_OK, or a status the writer passes back unchanged.
 *   ::GTEXT_INI_E_WRITE says the destination refused; ::GTEXT_INI_E_OOM keeps
 *   an allocation failure inside a sink distinguishable from one.
 */
typedef GTEXT_INI_Status (*GTEXT_INI_Write_Function)(void * user,
    const char * bytes, size_t len);

/**
 * @struct GTEXT_INI_Sink
 * @brief Where written bytes go.
 */
typedef struct {
  GTEXT_INI_Write_Function write; ///< Called for each chunk.
  void * user;                    ///< Passed back unchanged.
} GTEXT_INI_Sink;

/**
 * @brief Point @p sink at a fresh growable buffer.
 *
 * The buffer comes from the default allocator rather than a write option's,
 * because a sink is created before any options are seen and outlives the write.
 *
 * @param sink Receives the sink. Must not be NULL.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_INVALID or ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_sink_buffer(GTEXT_INI_Sink * sink);

/**
 * @brief The bytes a growable buffer sink has collected.
 *
 * NUL-terminated for convenience; gtext_ini_sink_buffer_size() is
 * authoritative, since a value may contain a NUL.
 *
 * @param sink The sink.
 * @return The bytes, or NULL.
 */
GTEXT_API const char * gtext_ini_sink_buffer_data(
    const GTEXT_INI_Sink * sink);

/**
 * @brief How many bytes a growable buffer sink holds.
 *
 * @param sink The sink.
 * @return The length.
 */
GTEXT_API size_t gtext_ini_sink_buffer_size(const GTEXT_INI_Sink * sink);

/**
 * @brief Release a growable buffer sink.
 *
 * @param sink The sink. NULL is ignored.
 */
GTEXT_API void gtext_ini_sink_buffer_free(GTEXT_INI_Sink * sink);

/**
 * @struct GTEXT_INI_Write_Options
 * @brief What a write is allowed to do.
 *
 * Zeroed is the default, which is the byte-preserving one. Use
 * gtext_ini_write_options_default() anyway, so that a field added later
 * arrives with its intended value rather than with zero.
 */
typedef struct {
  /**
   * The allocator the writer's own working memory comes from, or NULL for the
   * default. Not the sink's: a buffer sink owns its buffer.
   */
  const GTEXT_Allocator * allocator;

  /**
   * Emit `key=value` for every entry rather than reproducing its original
   * line, and drop the original spacing. Default false.
   *
   * With this set, a parse-write round trip is no longer byte-identical, and
   * that is the point of the option: a caller normalizing a file on purpose
   * wants the difference, and a caller rewriting one in place does not.
   */
  bool normalize;

  /**
   * Emit the comment and blank lines the tree carries. Default true.
   *
   * False produces a document with the same data and none of its comments,
   * which for the Desktop Entry dialect is a departure from §3's preservation
   * requirement - available because a caller may be generating a file rather
   * than rewriting one, and named so that doing it is deliberate.
   */
  bool emit_comments;

  /**
   * Write CRLF rather than LF. Default false.
   *
   * Desktop Entry §3 specifies linefeed, so this is off; it exists because the
   * generic dialect accepts CRLF on input and a caller round-tripping such a
   * file may want to keep it.
   */
  bool crlf;
} GTEXT_INI_Write_Options;

/**
 * @brief The defaults: allocator NULL, no normalizing, comments emitted, LF.
 *
 * @return The defaults, by value.
 */
GTEXT_API GTEXT_INI_Write_Options gtext_ini_write_options_default(void);

/**
 * @brief Write a document to a sink.
 *
 * @param doc The document.
 * @param sink Where the bytes go.
 * @param opts Options, or NULL for gtext_ini_write_options_default().
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_UNREPRESENTABLE,
 *   ::GTEXT_INI_E_INVALID, ::GTEXT_INI_E_OOM, or whatever the sink returned.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_write(const GTEXT_INI_Document * doc,
    GTEXT_INI_Sink * sink, const GTEXT_INI_Write_Options * opts);

/**
 * @brief Write a document to a file, atomically.
 *
 * Writes a temporary beside the destination and renames it into place, so a
 * reader never sees a half-written file.
 *
 * @param doc The document.
 * @param path The destination.
 * @param opts Options, or NULL for the defaults.
 * @return ::GTEXT_INI_OK, or a failure as gtext_ini_write() reports it.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_write_file(const GTEXT_INI_Document * doc,
    const char * path, const GTEXT_INI_Write_Options * opts);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_INI_INI_WRITER_H
