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
 * @file ini_file_io.c
 * @brief Reading an INI document from a file and writing one back atomically.
 */

#include "../text_file_io_internal.h"
#include "ini_internal.h"

/** Map a shared file status onto this module's codes. */
static GTEXT_INI_Status ini_file_error(gtext_file_status status,
    GTEXT_INI_Error * err) {
  GTEXT_INI_Status code;
  const char * message;
  switch (status) {
    case GTEXT_FILE_E_OPEN:
      code = GTEXT_INI_E_INVALID;
      message = "could not open the file";
      break;
    case GTEXT_FILE_E_READ:
      code = GTEXT_INI_E_INVALID;
      message = "the file could not be read to the end";
      break;
    case GTEXT_FILE_E_WRITE:
      code = GTEXT_INI_E_WRITE;
      message = "the file could not be written";
      break;
    case GTEXT_FILE_E_OOM:
      code = GTEXT_INI_E_OOM;
      message = "out of memory reading the file";
      break;
    case GTEXT_FILE_E_LIMIT:
      code = GTEXT_INI_E_LIMIT;
      message = "the file is larger than max_total_bytes";
      break;
    default:
      code = GTEXT_INI_E_INVALID;
      message = "the file could not be used";
      break;
  }
  if (err) gtext_ini_set_error(err, code, message, NULL, 0, 0);
  return code;
}

GTEXT_INI_Document * gtext_ini_parse_file(const char * path,
    const GTEXT_INI_Parse_Options * opts, GTEXT_INI_Error * err) {
  if (err) memset(err, 0, sizeof(*err));
  if (!path) {
    gtext_ini_set_error(err, GTEXT_INI_E_INVALID, "path is NULL", NULL, 0, 0);
    return NULL;
  }
  GTEXT_INI_Parse_Options effective = opts ? *opts
                                          : gtext_ini_parse_options_default();
  char * data = NULL;
  size_t len = 0;
  gtext_file_status fs =
      gtext_file_read_all(path, effective.max_total_bytes,
          effective.allocator, &data, &len);
  if (fs != GTEXT_FILE_OK) {
    ini_file_error(fs, err);
    return NULL;
  }
  GTEXT_INI_Document * doc = gtext_ini_parse(data, len, &effective, err);
  gtext_file_free(effective.allocator, data);
  return doc;
}

/** What gtext_ini_write_file() carries through the atomic write. */
typedef struct {
  const GTEXT_INI_Document * doc;
  const GTEXT_INI_Write_Options * opts;
  gtext_file_write_cb write;
  void * write_user;
  GTEXT_INI_Status status;
} ini_file_write_ctx;

static GTEXT_INI_Status ini_file_sink_write(void * user, const char * bytes,
    size_t len) {
  ini_file_write_ctx * ctx = user;
  if (ctx->write(ctx->write_user, bytes, len) != 0) return GTEXT_INI_E_WRITE;
  return GTEXT_INI_OK;
}

static int ini_file_emit(void * user, gtext_file_write_cb write,
    void * write_user) {
  ini_file_write_ctx * ctx = user;
  ctx->write = write;
  ctx->write_user = write_user;
  GTEXT_INI_Sink sink;
  sink.write = ini_file_sink_write;
  sink.user = ctx;
  ctx->status = gtext_ini_write(ctx->doc, &sink, ctx->opts);
  return ctx->status == GTEXT_INI_OK ? 0 : -1;
}

GTEXT_INI_Status gtext_ini_write_file(const GTEXT_INI_Document * doc,
    const char * path, const GTEXT_INI_Write_Options * opts) {
  if (!doc || !path) return GTEXT_INI_E_INVALID;
  ini_file_write_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.doc = doc;
  ctx.opts = opts;
  ctx.status = GTEXT_INI_OK;
  /* opts may be NULL - the writer fills in the defaults - and NULL is the
     default allocator either way, so no options object is built just to read
     one field off it. */
  gtext_file_status fs = gtext_file_write_atomic(
      path, opts ? opts->allocator : NULL, ini_file_emit, &ctx);
  /* The writer's own refusal is the more specific answer; the file layer only
   * knows that the callback failed, and an unrepresentable value must not come
   * back as a disk error. */
  if (ctx.status != GTEXT_INI_OK) return ctx.status;
  if (fs != GTEXT_FILE_OK) return ini_file_error(fs, NULL);
  return GTEXT_INI_OK;
}
