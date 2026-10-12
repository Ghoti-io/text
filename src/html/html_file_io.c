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
 * HTML file I/O: reading a document from a file.
 *
 * The same shared file layer, and the same mapping of its statuses, as src/xml/xml_file_io.c,
 * src/json/json_file_io.c and src/csv/csv_file_io.c.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "../text_file_io_internal.h"
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
