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
 * @file toml_file_io.c
 * @brief Reading a TOML document from a file.
 */

#include "../text_file_io_internal.h"
#include "toml_internal.h"
#include <string.h>

/** Map a shared file status onto this module's codes. */
static void toml_file_error(gtext_file_status status, GTEXT_TOML_Error * err) {
  if (!err) return;
  GTEXT_TOML_Status code;
  const char * message;
  switch (status) {
    case GTEXT_FILE_E_OPEN:
      code = GTEXT_TOML_E_INVALID;
      message = "could not open the file";
      break;
    case GTEXT_FILE_E_READ:
      code = GTEXT_TOML_E_INVALID;
      message = "the file could not be read to the end";
      break;
    case GTEXT_FILE_E_WRITE:
      code = GTEXT_TOML_E_WRITE;
      message = "the file could not be written";
      break;
    case GTEXT_FILE_E_OOM:
      code = GTEXT_TOML_E_OOM;
      message = "out of memory reading the file";
      break;
    case GTEXT_FILE_E_LIMIT:
      code = GTEXT_TOML_E_LIMIT;
      message = "the file is longer than max_total_bytes allows";
      break;
    default:
      code = GTEXT_TOML_E_INVALID;
      message = "the file operation failed";
      break;
  }
  memset(err, 0, sizeof(*err));
  err->code = code;
  err->message = message;
  err->line = 1;
  err->col = 1;
}

GTEXT_TOML_Value * gtext_toml_parse_file(const char * path,
    const GTEXT_TOML_Parse_Options * opts, GTEXT_TOML_Error * err) {
  if (err) memset(err, 0, sizeof(*err));
  if (!path) {
    if (err) {
      err->code = GTEXT_TOML_E_INVALID;
      err->message = "no path";
      err->line = 1;
      err->col = 1;
    }
    return NULL;
  }

  GTEXT_TOML_Parse_Options effective = gtext_toml_parse_options_default();
  if (opts) effective = *opts;

  char * data = NULL;
  size_t len = 0;
  /* max_total_bytes reaches the reader rather than being applied afterwards,
   * so an over-large file is refused without first being held in memory. The
   * YAML module read the whole file and then checked, which made the option a
   * statement about what it would parse rather than about what it would
   * allocate. */
  gtext_file_status fs =
      gtext_file_read_all(path, effective.max_total_bytes, &data, &len);
  if (fs != GTEXT_FILE_OK) {
    toml_file_error(fs, err);
    return NULL;
  }
  GTEXT_TOML_Value * value = gtext_toml_parse(data, len, &effective, err);
  gtext_file_free(data);
  return value;
}
