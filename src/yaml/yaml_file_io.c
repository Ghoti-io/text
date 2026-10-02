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
 * YAML file I/O helpers.
 *
 * The reading and writing themselves live in src/text_file_io.c, which is a
 * seam onto ghoti.io-cutil's file module. This file used to carry its own copy
 * of all of it - a whole-file reader, a temporary-file creator with a
 * `#ifdef _MSC_VER` arm, and a rename wrapper - which is how the copy came to
 * differ from the one JSON and CSV share:
 *
 * - it read with fseek/ftell/fread, so a pipe, a FIFO, /dev/stdin or anything
 *   under /proc was refused with "Failed to seek file". That is the one
 *   difference a caller could see, and there is a test for it;
 * - it never applied GTEXT_YAML_Parse_Options::max_total_bytes to the file,
 *   so an over-large document was read into memory in full and refused
 *   afterwards by the parser. The answer was the same either way -
 *   GTEXT_YAML_E_LIMIT - which is why nothing caught it: what was wrong was
 *   that the limit had already been spent by the time it was applied;
 * - its temporary-file creator did not remove the file it had just created if
 *   fdopen() failed, where the shared one does. That path is close to
 *   unreachable, so this is a difference between two copies rather than a bug
 *   anyone met.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/yaml/yaml_core.h>
#include <ghoti.io/text/yaml/yaml_dom.h>
#include <ghoti.io/text/yaml/yaml_writer.h>

#include "../text_file_io_internal.h"
#include "yaml_internal.h"

/**
 * @brief Turn a shared file status into a YAML status, and describe it.
 *
 * The distinctions the shared layer draws are the ones worth keeping: a limit
 * is not an I/O error and running out of memory is not a malformed document.
 * Before this, every one of them arrived as GTEXT_YAML_E_INVALID.
 */
static GTEXT_YAML_Status yaml_file_error(
    gtext_file_status status, GTEXT_YAML_Error * err) {
  GTEXT_YAML_Status code;
  const char * message;

  switch (status) {
  case GTEXT_FILE_E_OPEN:
    code = GTEXT_YAML_E_INVALID;
    message = "Failed to open file";
    break;
  case GTEXT_FILE_E_READ:
    code = GTEXT_YAML_E_INVALID;
    message = "Failed to read file contents";
    break;
  case GTEXT_FILE_E_WRITE:
    code = GTEXT_YAML_E_WRITE;
    message = "Failed to write YAML file";
    break;
  case GTEXT_FILE_E_OOM:
    code = GTEXT_YAML_E_OOM;
    message = "Out of memory reading file";
    break;
  case GTEXT_FILE_E_LIMIT:
    code = GTEXT_YAML_E_LIMIT;
    message = "File exceeds max_total_bytes";
    break;
  default:
    code = GTEXT_YAML_E_INVALID;
    message = "File operation failed";
    break;
  }
  if (err) {
    err->code = code;
    err->message = message;
  }
  return code;
}

static const char *detect_input_newline(const char *buffer, size_t len) {
  if (!buffer || len == 0) {
    return NULL;
  }

  for (size_t i = 0; i < len; i++) {
    if (buffer[i] == '\r') {
      if (i + 1 < len && buffer[i + 1] == '\n') {
        return "\r\n";
      }
      return "\r";
    }
    if (buffer[i] == '\n') {
      return "\n";
    }
  }

  return NULL;
}

static void set_document_newline(GTEXT_YAML_Document *doc, const char *newline) {
  if (!doc || !newline) {
    return;
  }
  doc->input_newline = newline;
}

/**
 * @brief Read a whole YAML file, applying the caller's size limit while it is
 *        still a limit rather than a diagnosis.
 */
static gtext_file_status yaml_file_slurp(const char * path,
    const GTEXT_YAML_Parse_Options * options,
    GTEXT_YAML_Parse_Options * out_effective, char ** out_data,
    size_t * out_len) {
  /* The same resolution the parser itself does, rather than a second copy of
   * the rule - which is why this needed no change when a max_total_bytes of 0
   * stopped meaning "no limit" and started meaning "the default", as the
   * public header had said all along. */
  *out_effective = gtext_yaml_parse_options_effective(options);
  return gtext_file_read_all(path, out_effective->max_total_bytes,
      out_effective->allocator, out_data, out_len);
}

GTEXT_API GTEXT_YAML_Document * gtext_yaml_parse_file(
  const char * path,
  const GTEXT_YAML_Parse_Options * options,
  GTEXT_YAML_Error * out_err
) {
  if (!path) {
    if (out_err) {
      out_err->code = GTEXT_YAML_E_INVALID;
      out_err->message = "Path is NULL";
    }
    return NULL;
  }

  GTEXT_YAML_Parse_Options effective;
  char * buffer = NULL;
  size_t len = 0;
  gtext_file_status fs =
      yaml_file_slurp(path, options, &effective, &buffer, &len);
  if (fs != GTEXT_FILE_OK) {
    yaml_file_error(fs, out_err);
    return NULL;
  }

  const char *newline = detect_input_newline(buffer, len);
  GTEXT_YAML_Document * doc =
      gtext_yaml_parse(buffer, len, &effective, out_err);
  set_document_newline(doc, newline);
  gtext_file_free(effective.allocator, buffer);
  return doc;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_parse_file_all(
  const char * path,
  const GTEXT_YAML_Parse_Options * options,
  GTEXT_YAML_Document *** out_docs,
  size_t * out_count,
  GTEXT_YAML_Error * out_err
) {
  if (!path || !out_docs || !out_count) {
    if (out_err) {
      out_err->code = GTEXT_YAML_E_INVALID;
      out_err->message = "Invalid arguments";
    }
    return GTEXT_YAML_E_INVALID;
  }

  GTEXT_YAML_Parse_Options effective;
  char * buffer = NULL;
  size_t len = 0;
  gtext_file_status fs =
      yaml_file_slurp(path, options, &effective, &buffer, &len);
  if (fs != GTEXT_FILE_OK) {
    return yaml_file_error(fs, out_err);
  }

  const char *newline = detect_input_newline(buffer, len);
  size_t count = 0;
  GTEXT_YAML_Document ** docs =
      gtext_yaml_parse_all(buffer, len, &count, &effective, out_err);
  gtext_file_free(effective.allocator, buffer);
  if (!docs) {
    return out_err ? out_err->code : GTEXT_YAML_E_INVALID;
  }

  for (size_t i = 0; i < count; i++) {
    set_document_newline(docs[i], newline);
  }

  *out_docs = docs;
  *out_count = count;
  return GTEXT_YAML_OK;
}

/** Context for the atomic write callback. */
typedef struct {
  const GTEXT_YAML_Document * doc;
  const GTEXT_YAML_Write_Options * opts;
  GTEXT_YAML_Status status;
} yaml_file_write_ctx;

static int yaml_file_emit(
    void * ctx, gtext_file_write_cb write, void * write_user) {
  yaml_file_write_ctx * state = (yaml_file_write_ctx *)ctx;

  GTEXT_YAML_Sink sink;
  sink.write = write;
  sink.user = write_user;

  state->status = gtext_yaml_write_document(state->doc, &sink, state->opts);
  return state->status == GTEXT_YAML_OK ? 0 : 1;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_write_file(
  const char * path,
  const GTEXT_YAML_Document * doc,
  const GTEXT_YAML_Write_Options * options,
  GTEXT_YAML_Error * out_err
) {
  if (!path || !doc) {
    if (out_err) {
      out_err->code = GTEXT_YAML_E_INVALID;
      out_err->message = "Invalid arguments";
    }
    return GTEXT_YAML_E_INVALID;
  }

  GTEXT_YAML_Write_Options local_opts;
  const GTEXT_YAML_Write_Options *use_opts = options;
  if (!use_opts) {
    local_opts = gtext_yaml_write_options_default();
    if (doc->input_newline) {
      local_opts.newline = doc->input_newline;
    }
    use_opts = &local_opts;
  } else if (!use_opts->newline && doc->input_newline) {
    local_opts = *use_opts;
    local_opts.newline = doc->input_newline;
    use_opts = &local_opts;
  }

  yaml_file_write_ctx ctx;
  ctx.doc = doc;
  ctx.opts = use_opts;
  ctx.status = GTEXT_YAML_OK;

  gtext_file_status fs =
      gtext_file_write_atomic(path, use_opts->allocator, yaml_file_emit, &ctx);
  if (fs != GTEXT_FILE_OK) {
    /* A failure inside the serializer is the more specific answer, and it is
     * the one the caller can act on; the shared layer only knows that the
     * callback said no. */
    if (ctx.status != GTEXT_YAML_OK) {
      if (out_err) {
        out_err->code = ctx.status;
        out_err->message = "Failed to serialize YAML document";
      }
      return ctx.status;
    }
    return yaml_file_error(fs, out_err);
  }

  return GTEXT_YAML_OK;
}
