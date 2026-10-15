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
 * XML file I/O: reading a document from a file, writing one atomically.
 *
 * The same shared file layer, and the same mapping of its statuses, as
 * src/json/json_file_io.c and src/csv/csv_file_io.c.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "../text_file_io_internal.h"
#include "xml_internal.h"

/** Map a shared file status onto this module's codes and message. */
static GTEXT_XML_Status xml_file_status(
    gtext_file_status status, const char ** message) {
  switch (status) {
  case GTEXT_FILE_E_NOT_FOUND:
    *message = "No such file or directory";
    return GTEXT_XML_E_INVALID;
  case GTEXT_FILE_E_ACCESS:
    *message = "Permission denied opening file";
    return GTEXT_XML_E_INVALID;
  case GTEXT_FILE_E_OPEN:
    *message = "Failed to open file";
    return GTEXT_XML_E_INVALID;
  case GTEXT_FILE_E_READ:
    *message = "Failed to read file";
    return GTEXT_XML_E_INVALID;
  case GTEXT_FILE_E_WRITE:
    *message = "Failed to write file";
    return GTEXT_XML_E_WRITE;
  case GTEXT_FILE_E_OOM:
    *message = "Out of memory reading file";
    return GTEXT_XML_E_OOM;
  case GTEXT_FILE_E_LIMIT:
    *message = "File exceeds max_input_bytes";
    return GTEXT_XML_E_LIMIT_INPUT;
  default:
    *message = "File operation failed";
    return GTEXT_XML_E_INVALID;
  }
}

GTEXT_XML_Status gtext_xml_parse_file(const char * path,
    const GTEXT_XML_Parse_Options * opts, GTEXT_Markup_Document ** out,
    GTEXT_XML_Error * err) {
  if (err) {
    memset(err, 0, sizeof(*err));
  }
  if (!path || !out) {
    if (err) {
      err->code = GTEXT_XML_E_INVALID;
      err->message = !path ? "Path must not be NULL" : "out must not be NULL";
      err->line = 1;
      err->column = 1;
    }
    return GTEXT_XML_E_INVALID;
  }

  const GTEXT_XML_Parse_Options effective =
      opts ? *opts : gtext_xml_parse_options_default();
  const GTEXT_XML_Status option_status =
      xml_validate_dtd_options(&effective, err);
  if (option_status != GTEXT_XML_OK) {
    return option_status;
  }
  /* The limit is applied while reading, so a file far larger than the caller
     allows is refused without being held in memory first. */
  const size_t max_bytes = effective.max_input_bytes
      ? effective.max_input_bytes : XML_DEFAULT_MAX_INPUT;

  char * data = NULL;
  size_t len = 0;
  const gtext_file_status fs =
      gtext_file_read_all(path, max_bytes, effective.allocator, &data, &len);
  if (fs != GTEXT_FILE_OK) {
    const char * message = NULL;
    const GTEXT_XML_Status code = xml_file_status(fs, &message);
    if (err) {
      err->code = code;
      err->message = message;
      err->line = 1;
      err->column = 1;
    }
    return code;
  }

  const GTEXT_XML_Status status =
      gtext_xml_parse(data, len, &effective, out, err);
  gtext_file_free(effective.allocator, data);
  return status;
}

/* ---- writing ------------------------------------------------------------ */

typedef struct {
  const GTEXT_Markup_Document * doc;
  const GTEXT_XML_Write_Options * opts;
  GTEXT_XML_Write_Result * result;
  GTEXT_XML_Status status;
} xml_file_write_ctx;

typedef struct {
  gtext_file_write_cb write;
  void * user;
} xml_file_adapter;

/* The file layer's sink returns 0 on success; this module's returns a status. */
static GTEXT_Markup_Status xml_file_sink_write(
    void * user, const char * bytes, size_t len) {
  const xml_file_adapter * adapter = user;
  return adapter->write(adapter->user, bytes, len) == 0
      ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_WRITE;
}

static int xml_file_emit(
    void * ctx, gtext_file_write_cb write, void * write_user) {
  xml_file_write_ctx * c = ctx;
  xml_file_adapter adapter = {write, write_user};
  const GTEXT_Markup_Sink sink = {xml_file_sink_write, &adapter};
  c->status = gtext_xml_write(&sink, c->doc, c->opts, c->result);
  return c->status == GTEXT_XML_OK ? 0 : 1;
}

GTEXT_XML_Status gtext_xml_write_file(const char * path,
    const GTEXT_Markup_Document * doc, const GTEXT_XML_Write_Options * opts,
    GTEXT_XML_Write_Result * result) {
  if (result) {
    memset(result, 0, sizeof(*result));
  }
  if (!path || !doc) {
    if (result) {
      result->code = GTEXT_XML_E_INVALID;
      result->message = "Path and document must not be NULL";
    }
    return GTEXT_XML_E_INVALID;
  }
  /* The tree is checked first, so that a refused one creates nothing and its
     refusal is not hidden behind a file error. */
  const GTEXT_XML_Status checked = xml_write_check(doc, opts, result);
  if (checked != GTEXT_XML_OK) {
    return checked;
  }
  xml_file_write_ctx ctx = {doc, opts, result, GTEXT_XML_OK};
  const GTEXT_Allocator * alloc = opts && opts->allocator
      ? opts->allocator : &doc->allocator;
  const gtext_file_status fs =
      gtext_file_write_atomic(path, alloc, xml_file_emit, &ctx);
  if (fs == GTEXT_FILE_OK) {
    return GTEXT_XML_OK;
  }
  /* A failure of the serializer is more specific than "write failed". */
  if (ctx.status != GTEXT_XML_OK && ctx.status != GTEXT_XML_E_WRITE) {
    return ctx.status;
  }
  const char * message = NULL;
  const GTEXT_XML_Status code = xml_file_status(fs, &message);
  if (result) {
    result->code = code;
    result->message = message;
    result->path[0] = '\0';
  }
  return code;
}
