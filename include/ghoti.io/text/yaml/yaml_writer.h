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
 * @file yaml_writer.h
 * @brief Document emission and sink helpers for YAML.
 *
 * The writer API serializes a DOM into a caller-provided sink. The
 * sink abstraction mirrors other modules in the repository and allows
 * writing to growable buffers, fixed buffers, or custom callbacks.
 */

#ifndef GHOTI_IO_GTEXT_YAML_YAML_WRITER_H
#define GHOTI_IO_GTEXT_YAML_YAML_WRITER_H

#include <ghoti.io/text/yaml/yaml_core.h>
#include <ghoti.io/text/yaml/yaml_stream.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write callback function type.
 *
 * This callback is invoked by the writer to output YAML data chunks.
 * Return 0 on success, non-zero on error.
 */
typedef int (*GTEXT_YAML_Write_Function)(
	void * user,
	const char * bytes,
	size_t len
);

/**
 * @brief YAML output sink structure.
 */
typedef struct {
	GTEXT_YAML_Write_Function write; ///< Called with each chunk of output
	void * user;                     ///< Passed to `write`
} GTEXT_YAML_Sink;

/**
 * @brief Growable buffer sink structure.
 */
typedef struct {
	char * data;  ///< The buffer, NUL-terminated
	size_t size;  ///< Bytes allocated
	size_t used;  ///< Bytes written, not counting the terminator
} GTEXT_YAML_Buffer_Sink;

/**
 * @brief Fixed buffer sink structure.
 */
typedef struct {
	char * data;     ///< The buffer, NUL-terminated
	size_t size;     ///< Capacity in bytes, including the terminator
	size_t used;     ///< Bytes written, not counting the terminator
	bool truncated;  ///< Set when output did not fit and was cut short
} GTEXT_YAML_Fixed_Buffer_Sink;

/**
 * @brief Create a growable buffer sink.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_sink_buffer(GTEXT_YAML_Sink * sink);

/**
 * @brief Get the buffer data from a growable buffer sink.
 */
GTEXT_API const char * gtext_yaml_sink_buffer_data(
	const GTEXT_YAML_Sink * sink
);

/**
 * @brief Get the buffer size from a growable buffer sink.
 */
GTEXT_API size_t gtext_yaml_sink_buffer_size(
	const GTEXT_YAML_Sink * sink
);

/**
 * @brief Free a growable buffer sink.
 */
GTEXT_API void gtext_yaml_sink_buffer_free(GTEXT_YAML_Sink * sink);

/**
 * @brief Create a fixed-size buffer sink.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_sink_fixed_buffer(
	GTEXT_YAML_Sink * sink,
	char * buffer,
	size_t size
);

/**
 * @brief Get the number of bytes written to a fixed buffer sink.
 */
GTEXT_API size_t gtext_yaml_sink_fixed_buffer_used(
	const GTEXT_YAML_Sink * sink
);

/**
 * @brief Check if truncation occurred in a fixed buffer sink.
 */
GTEXT_API bool gtext_yaml_sink_fixed_buffer_truncated(
	const GTEXT_YAML_Sink * sink
);

/**
 * @brief Free a fixed buffer sink.
 */
GTEXT_API void gtext_yaml_sink_fixed_buffer_free(GTEXT_YAML_Sink * sink);

/**
 * @brief Serialize @p doc to @p sink using @p opts.
 *
 * If @p opts is NULL default write options are used. The function returns
 * GTEXT_YAML_OK on success or a writer/sink error (GTEXT_YAML_E_WRITE).
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_write_document(const GTEXT_YAML_Document * doc, GTEXT_YAML_Sink * sink, const GTEXT_YAML_Write_Options * opts);

/**
 * @brief Serialize multiple documents to @p sink using @p opts.
 *
 * This emits explicit document start markers for each document.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_write_documents(
	GTEXT_YAML_Document * const * docs,
	size_t count,
	GTEXT_YAML_Sink * sink,
	const GTEXT_YAML_Write_Options * opts
);

/**
 * @brief Forward declaration of streaming YAML writer structure.
 */
typedef struct GTEXT_YAML_Writer GTEXT_YAML_Writer;

/**
 * @brief Create a new streaming YAML writer.
 */
GTEXT_API GTEXT_YAML_Writer * gtext_yaml_writer_new(
	GTEXT_YAML_Sink sink,
	const GTEXT_YAML_Write_Options * opts
);

/**
 * @brief Free a streaming YAML writer.
 */
GTEXT_API void gtext_yaml_writer_free(GTEXT_YAML_Writer * writer);

/**
 * @brief Feed a streaming event to the writer.
 *
 * The writer takes **composed** events: a collection is a SEQUENCE_START or
 * MAPPING_START, its children, and the matching end. `gtext_yaml_stream_walk()`
 * produces exactly that shape from a parsed document.
 *
 * The streaming *parser* does not. For a block collection it reports the `:`
 * and the `-` as GTEXT_YAML_EVENT_INDICATOR and leaves composing to its
 * consumer, so `gtext_yaml_stream_*` and this function are not two ends of a
 * pipe however alike their types look. An INDICATOR event is refused with
 * GTEXT_YAML_E_INVALID rather than ignored: it used to be answered with OK
 * and nothing written, which gave a caller who joined the two no error and a
 * document with its block structure gone.
 *
 * DIRECTIVE and COMMENT events are written where they stand. A directive
 * belongs to the document it precedes (9.2) and is refused inside one.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_writer_event(
	GTEXT_YAML_Writer * writer,
	const GTEXT_YAML_Event * event
);

/**
 * @brief Finish writing and validate writer state.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_writer_finish(GTEXT_YAML_Writer * writer);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_YAML_YAML_WRITER_H
