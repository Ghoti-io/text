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
 * CSV file I/O.
 */

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/csv/csv_core.h>
#include <ghoti.io/text/csv/csv_table.h>
#include <ghoti.io/text/csv/csv_writer.h>
#include <ghoti.io/text/macros.h>

#include "../text_file_io_internal.h"
#include "csv_internal.h"
#include "csv_stream_internal.h"

/** Map a shared file status onto this module's codes and message. */
static GTEXT_CSV_Status csv_file_error(
    gtext_file_status status, GTEXT_CSV_Error * err) {
  GTEXT_CSV_Status code;
  const char * message;
  switch (status) {
  case GTEXT_FILE_E_NOT_FOUND:
    code = GTEXT_CSV_E_INVALID;
    message = "No such file or directory";
    break;
  case GTEXT_FILE_E_ACCESS:
    code = GTEXT_CSV_E_INVALID;
    message = "Permission denied opening file";
    break;
  case GTEXT_FILE_E_OPEN:
    code = GTEXT_CSV_E_INVALID;
    message = "Failed to open file";
    break;
  case GTEXT_FILE_E_READ:
    code = GTEXT_CSV_E_INVALID;
    message = "Failed to read file";
    break;
  case GTEXT_FILE_E_WRITE:
    code = GTEXT_CSV_E_WRITE;
    message = "Failed to write file";
    break;
  case GTEXT_FILE_E_OOM:
    code = GTEXT_CSV_E_OOM;
    message = "Out of memory reading file";
    break;
  case GTEXT_FILE_E_LIMIT:
    code = GTEXT_CSV_E_LIMIT;
    message = "File exceeds max_total_bytes";
    break;
  default:
    code = GTEXT_CSV_E_INVALID;
    message = "File operation failed";
    break;
  }
  CSV_SET_ERROR(err, code, message);
  return code;
}

GTEXT_API GTEXT_CSV_Table * gtext_csv_parse_file(const char * path,
    const GTEXT_CSV_Parse_Options * opts, GTEXT_CSV_Error * err) {
  if (!path) {
    CSV_SET_ERROR(err, GTEXT_CSV_E_INVALID, "Path must not be NULL");
    return NULL;
  }

  // The size limit is applied while reading rather than after, so a file far
  // larger than the caller allows is refused without being held in memory
  // first.
  GTEXT_CSV_Parse_Options effective =
      opts ? *opts : gtext_csv_parse_options_default();
  size_t max_bytes = csv_get_limit(
      effective.max_total_bytes, CSV_DEFAULT_MAX_TOTAL_BYTES);

  char * data = NULL;
  size_t len = 0;
  gtext_file_status fs = gtext_file_read_all(
      path, max_bytes, effective.allocator, &data, &len);
  if (fs != GTEXT_FILE_OK) {
    csv_file_error(fs, err);
    return NULL;
  }

  // in_situ_mode would hand back fields pointing into a buffer this function
  // owns and is about to free, so it cannot be honored here.
  effective.in_situ_mode = false;

  GTEXT_CSV_Table * table = gtext_csv_parse_table(data, len, &effective, err);
  gtext_file_free(effective.allocator, data);
  return table;
}

/** Context for the atomic write callback. */
typedef struct {
  const GTEXT_CSV_Table * table;
  const GTEXT_CSV_Write_Options * opts;
  GTEXT_CSV_Status status;
} csv_file_write_ctx;

static GTEXT_CSV_Status csv_file_sink_write(
    void * user, const char * bytes, size_t len) {
  // The shared layer's sink is (user, bytes, len) returning 0 on success; this
  // module's sink returns a status. Adapt between them.
  struct {
    gtext_file_write_cb write;
    void * user;
  } * adapter = user;
  return adapter->write(adapter->user, bytes, len) == 0 ? GTEXT_CSV_OK
                                                        : GTEXT_CSV_E_WRITE;
}

static int csv_file_emit(
    void * ctx, gtext_file_write_cb write, void * write_user) {
  csv_file_write_ctx * c = (csv_file_write_ctx *)ctx;
  struct {
    gtext_file_write_cb write;
    void * user;
  } adapter = {write, write_user};

  GTEXT_CSV_Sink sink;
  sink.write = csv_file_sink_write;
  sink.user = &adapter;

  c->status = gtext_csv_write_table(&sink, c->opts, c->table);
  return c->status == GTEXT_CSV_OK ? 0 : 1;
}

GTEXT_API GTEXT_CSV_Status gtext_csv_write_file(const char * path,
    const GTEXT_CSV_Table * table, const GTEXT_CSV_Write_Options * opts,
    GTEXT_CSV_Error * err) {
  if (!path || !table) {
    CSV_SET_ERROR(
        err, GTEXT_CSV_E_INVALID, "Path and table must not be NULL");
    return GTEXT_CSV_E_INVALID;
  }

  csv_file_write_ctx ctx;
  ctx.table = table;
  ctx.opts = opts;
  ctx.status = GTEXT_CSV_OK;

  /* opts may be NULL here - gtext_csv_write_table() fills in the defaults -
     and NULL is the default allocator either way, so no options object has to
     be built just to read one field off it. */
  gtext_file_status fs = gtext_file_write_atomic(
      path, opts ? opts->allocator : NULL, csv_file_emit, &ctx);
  if (fs != GTEXT_FILE_OK) {
    // A failure inside the serializer is more specific than "write failed".
    if (ctx.status != GTEXT_CSV_OK) {
      CSV_SET_ERROR(err, ctx.status, "Failed to serialize table");
      return ctx.status;
    }
    return csv_file_error(fs, err);
  }
  return GTEXT_CSV_OK;
}
