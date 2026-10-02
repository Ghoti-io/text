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
 * Shared file I/O helpers, internal to the library.
 *
 * Every format wants the same two things - read a whole document in, write a
 * whole document out without leaving a half-written file behind - and the
 * differences between them are in the parsing, not the plumbing. Keeping the
 * plumbing here means a fix to it reaches all three rather than one.
 *
 * The plumbing itself now lives in ghoti.io-cutil, which grew a file module
 * for this. What is left here is the seam: cutil's result codes turned into a
 * status the formats map onto their own, and the write-through-a-callback
 * shape the three writers stream into. YAML did not use this header and
 * carried its own copy of all of it, which is how it came to read with
 * fseek/ftell - so it could not read a pipe, and applied its own
 * max_total_bytes only after the file was already in memory.
 */

#ifndef GHOTI_IO_GTEXT_TEXT_FILE_IO_INTERNAL_H
#define GHOTI_IO_GTEXT_TEXT_FILE_IO_INTERNAL_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Why a file operation failed, without naming any format's status enum.
 *
 * Each module maps these onto its own codes, so the shared layer does not have
 * to know whether it is serving JSON, CSV or YAML.
 *
 * Several of these land on the *same* public status in every format and differ
 * only in the message - GTEXT_FILE_E_NOT_FOUND, _E_ACCESS, _E_OPEN and _E_READ
 * are all GTEXT_JSON_E_INVALID, for instance. That is deliberate and is worth
 * saying, because it is easy to read a five-way split as five public codes:
 * the public enums draw their distinctions where a caller can *act* on them,
 * and "the path is wrong" is one action whether the file is absent, forbidden
 * or unreadable. What differs is what the message tells the person reading it,
 * and a missing file and a forbidden one are not the same sentence.
 *
 * GTEXT_FILE_E_NOT_FOUND and GTEXT_FILE_E_ACCESS exist because cutil tells
 * them apart at the open and this library used to throw both away. A caller
 * pointed at a path it may not read was told "could not open the file", which
 * is true of every failure here and therefore says nothing.
 */
typedef enum {
  GTEXT_FILE_OK = 0,
  GTEXT_FILE_E_NOT_FOUND, ///< There is nothing at that path
  GTEXT_FILE_E_ACCESS,    ///< The filesystem refused on permission grounds
  GTEXT_FILE_E_OPEN,      ///< Could not open the path, for some other reason
  GTEXT_FILE_E_READ,      ///< Opened, but reading failed part way
  GTEXT_FILE_E_WRITE,     ///< Writing or committing failed
  GTEXT_FILE_E_OOM,       ///< Allocation failed
  GTEXT_FILE_E_LIMIT      ///< File larger than the caller allows
} gtext_file_status;

/**
 * @brief Read an entire file into a NUL-terminated heap buffer.
 *
 * Reads incrementally rather than seeking to the end first, so a pipe, a
 * FIFO, /dev/stdin and anything else without a size still work. The
 * terminator is written past @p out_len and is not counted in it; parsers here
 * all take an explicit length, but a terminator costs one byte and removes a
 * whole class of caller mistake.
 *
 * @param path      File to read.
 * @param max_bytes Refuse anything larger, or 0 for no limit. The limit is a
 *                  promise rather than a truncation: an over-large file is
 *                  refused, not shortened.
 * @param alloc     Allocator for the buffer, or NULL for the default. This is
 *                  the caller's allocator, and it has to be: the buffer is the
 *                  single largest allocation a *_parse_file() entry point
 *                  makes, so a parse that took it from anywhere else would
 *                  leave GTEXT_*_Parse_Options::allocator covering everything
 *                  except the file.
 * @param out_data  Receives the buffer; the caller releases it with
 *                  gtext_file_free(), not free(), **through the same
 *                  allocator**.
 * @param out_len   Receives the length in bytes, terminator excluded.
 * @return GTEXT_FILE_OK, or the reason it failed.
 */
GTEXT_INTERNAL_API gtext_file_status gtext_file_read_all(const char * path,
    size_t max_bytes, const GTEXT_Allocator * alloc, char ** out_data,
    size_t * out_len);

/**
 * @brief Release a buffer from gtext_file_read_all().
 *
 * A named function rather than free(), because the buffer comes from cutil and
 * is released through the allocator it was read with. It used to be the default
 * one, so free() would happen to work - exactly the kind of coincidence that
 * stops being true without anything failing to compile. Now that a caller can
 * supply one, free() here would be a free through the wrong allocator.
 *
 * @param alloc The allocator the buffer was read with, or NULL for the default.
 *              It must be the same one.
 * @param data  The buffer. NULL is accepted and ignored.
 */
GTEXT_INTERNAL_API void gtext_file_free(
    const GTEXT_Allocator * alloc, char * data);

/**
 * @brief Callback that writes one buffer, returning 0 on success.
 */
typedef int (*gtext_file_write_cb)(void * user, const char * bytes, size_t len);

/**
 * @brief Write a file atomically: fully replaced, or not touched at all.
 *
 * The content goes to a temporary file in the destination's own directory - so
 * that the rename stays on one filesystem, and is therefore atomic - is
 * committed to the disk, and only then replaces the destination. A caller
 * interrupted half way through, or a full disk, or a power loss, leaves the
 * previous file intact rather than truncated, which matters for exactly the
 * configuration files these parsers are usually pointed at.
 *
 * @param path    Destination path.
 * @param alloc   Allocator for the one buffer this needs - the destination's
 *                directory name, for placing the temporary file beside it - or
 *                NULL for the default. From the caller's write options, so a
 *                write to a file allocates where a write to a sink does.
 * @param emit    Called once with a sink to write through.
 * @param user    Passed back to @p emit.
 * @return GTEXT_FILE_OK, or the reason it failed.
 */
GTEXT_INTERNAL_API gtext_file_status gtext_file_write_atomic(const char * path,
    const GTEXT_Allocator * alloc,
    int (*emit)(void * ctx, gtext_file_write_cb write, void * write_user),
    void * user);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_TEXT_FILE_IO_INTERNAL_H
