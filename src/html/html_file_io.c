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
 * HTML file I/O: reading a document from a file, writing one atomically.
 *
 * The same shared file layer, and the same mapping of its statuses, as src/xml/xml_file_io.c,
 * src/json/json_file_io.c and src/csv/csv_file_io.c.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "../text_file_io_internal.h"
#include "../markup/markup_internal.h"
#include "html_api_internal.h"

/** Map a shared file status onto this module's codes and message. */
static GTEXT_HTML_Status html_file_status(gtext_file_status status, const char ** message) {
  switch (status) {
    case GTEXT_FILE_E_NOT_FOUND:
      *message = "No such file or directory";
      return GTEXT_HTML_E_INVALID;
    case GTEXT_FILE_E_ACCESS:
      *message = "Permission denied opening file";
      return GTEXT_HTML_E_INVALID;
    case GTEXT_FILE_E_OPEN:
      *message = "Failed to open file";
      return GTEXT_HTML_E_INVALID;
    case GTEXT_FILE_E_READ:
      *message = "Failed to read file";
      return GTEXT_HTML_E_INVALID;
    case GTEXT_FILE_E_WRITE:
      *message = "Failed to write file";
      return GTEXT_HTML_E_WRITE;
    case GTEXT_FILE_E_OOM:
      *message = "Out of memory reading file";
      return GTEXT_HTML_E_OOM;
    case GTEXT_FILE_E_LIMIT:
      *message = "File exceeds max_input_bytes";
      return GTEXT_HTML_E_LIMIT_INPUT;
    default:
      *message = "File operation failed";
      return GTEXT_HTML_E_INVALID;
  }
}

GTEXT_HTML_Status gtext_html_parse_file(const char * path, const GTEXT_HTML_Parse_Options * opts,
    GTEXT_Markup_Document ** out, GTEXT_HTML_Result * result) {
  html_result_clear(result);
  if (!path || !out) {
    html_result_set_failure(result, GTEXT_HTML_E_INVALID,
        !path ? "Path must not be NULL" : "out must not be NULL");
    return GTEXT_HTML_E_INVALID;
  }

  const GTEXT_HTML_Parse_Options effective = opts ? *opts : gtext_html_parse_options_default();
  /* The limit is applied while reading, so a file far larger than the caller allows is refused
     without being held in memory first. */
  const size_t max_bytes = effective.max_input_bytes ? effective.max_input_bytes
                                                     : GTEXT_HTML_TOKENIZER_DEFAULT_MAX_INPUT;

  char * data = NULL;
  size_t len = 0;
  const gtext_file_status fs = gtext_file_read_all(path, max_bytes, effective.allocator, &data, &len);
  if (fs != GTEXT_FILE_OK) {
    const char * message = NULL;
    const GTEXT_HTML_Status code = html_file_status(fs, &message);
    html_result_set_failure(result, code, message);
    return code;
  }

  const GTEXT_HTML_Status status = gtext_html_parse(data, len, &effective, out, result);
  gtext_file_free(effective.allocator, data);
  return status;
}

/* ---- writing ------------------------------------------------------------ */

/// What a file write carries to the callback of the file layer.
typedef struct {
  const GTEXT_Markup_Node * node;            ///< What to write.
  GTEXT_HTML_Serialize_Scope scope;          ///< ... how much of it.
  const GTEXT_HTML_Serialize_Options * opts; ///< The options.
  GTEXT_HTML_Serialize_Result * result;      ///< Where the outcome goes.
  GTEXT_HTML_Status status;                  ///< The serializer's status.
} html_file_write_ctx;

/// The file layer's writer, for the sink.
typedef struct {
  gtext_file_write_cb write; ///< The layer's function.
  void * user;               ///< Its argument.
} html_file_adapter;

/* The file layer's sink returns 0 on success; this module's returns a status. */
static GTEXT_Markup_Status html_file_sink_write(void * user, const char * bytes, size_t len) {
  const html_file_adapter * adapter = user;
  return adapter->write(adapter->user, bytes, len) == 0 ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_WRITE;
}

static int html_file_emit(void * ctx, gtext_file_write_cb write, void * write_user) {
  html_file_write_ctx * c = ctx;
  html_file_adapter adapter = {write, write_user};
  const GTEXT_Markup_Sink sink = {html_file_sink_write, &adapter};
  c->status = gtext_html_serialize(c->node, c->scope, c->opts, &sink, c->result);
  return c->status == GTEXT_HTML_OK ? 0 : 1;
}

GTEXT_HTML_Status gtext_html_serialize_file(const GTEXT_Markup_Node * node, GTEXT_HTML_Serialize_Scope scope,
    const char * path, const GTEXT_HTML_Serialize_Options * opts, GTEXT_HTML_Serialize_Result * result) {
  if (result) {
    memset(result, 0, sizeof(*result));
  }
  if (!path || !node) {
    if (result) {
      result->status = GTEXT_HTML_E_INVALID;
      result->message = "Path and node must not be NULL";
    }
    return GTEXT_HTML_E_INVALID;
  }
  /* The options are checked first, so that a bad request creates no file and is not hidden behind a
     file error. */
  {
    const GTEXT_HTML_Status st = html_serialize_check_options(opts);
    if (st != GTEXT_HTML_OK) {
      if (result) {
        result->status = st;
        result->message = gtext_html_status_string(st);
      }
      return st;
    }
  }
  html_file_write_ctx ctx = {node, scope, opts, result, GTEXT_HTML_OK};
  const GTEXT_Markup_Document * doc = gtext_markup_node_document(node);
  const GTEXT_Allocator * alloc = opts && opts->allocator ? opts->allocator : (doc ? &doc->allocator : NULL);
  if (!alloc) {
    if (result) {
      result->status = GTEXT_HTML_E_INVALID;
      result->message = "The node belongs to no document and no allocator was given";
    }
    return GTEXT_HTML_E_INVALID;
  }
  const gtext_file_status fs = gtext_file_write_atomic(path, alloc, html_file_emit, &ctx);
  if (fs == GTEXT_FILE_OK) {
    return GTEXT_HTML_OK;
  }
  /* The file was not replaced, so the result says why and counts nothing: the serializer may have finished and the
     commit failed, or stopped half way, and either way there is no output that exists. */
  GTEXT_HTML_Status code;
  const char * message = NULL;
  if (ctx.status != GTEXT_HTML_OK && ctx.status != GTEXT_HTML_E_WRITE) {
    code = ctx.status; /* a failure of the serializer is more specific than "write failed" */
    message = result && result->message ? result->message : gtext_html_status_string(code);
  }
  else {
    code = html_file_status(fs, &message);
  }
  if (result) {
    memset(result, 0, sizeof(*result));
    result->status = code;
    result->message = message;
  }
  return code;
}
