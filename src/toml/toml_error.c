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
 * @file toml_error.c
 * @brief Reporting where a TOML parse failed.
 */

#include "toml_internal.h"
#include <string.h>

/** How many bytes of the offending line to keep in the snippet. */
#define TOML_SNIPPET_MAX 120

void gtext_toml_error_free(GTEXT_TOML_Error * err) {
  if (!err) return;
  /* The allocator is not available here, and cannot be: the error struct is
   * the caller's, and a caller who passes one to a parse that failed before
   * reading its options has no allocator to name. Snippets are therefore
   * allocated through the default allocator, always, and this frees through
   * the same one. That is recorded in the header rather than left to be
   * discovered. */
  gtext_allocator_free(NULL, err->context_snippet);
  err->context_snippet = NULL;
  err->context_snippet_len = 0;
  err->caret_offset = 0;
}

/**
 * Line and column of an offset, counted from the start of the buffer.
 *
 * Recomputed rather than read from the parser's running line counter, because
 * a failure is reported at a position that is not always the position the
 * parser has reached: a duplicate key is refused where the key *began*, and
 * the parser is at the end of it by then. A counter that is right for one of
 * those and used for both is the kind of off-by-a-line nothing notices.
 */
static void locate(const char * buf, size_t len, size_t offset, int * line,
    int * col, size_t * line_start, size_t * line_end) {
  if (offset > len) offset = len;
  int l = 1;
  size_t start = 0;
  for (size_t i = 0; i < offset; ++i) {
    if (buf[i] == '\n') {
      ++l;
      start = i + 1;
    }
  }
  size_t end = start;
  while (end < len && buf[end] != '\n') ++end;
  /* Columns count characters, so a line of multi-byte text points at the right
   * one. Continuation bytes are 0b10xxxxxx and are not counted. */
  int c = 1;
  for (size_t i = start; i < offset; ++i) {
    if (((unsigned char) buf[i] & 0xC0) != 0x80) ++c;
  }
  *line = l;
  *col = c;
  *line_start = start;
  *line_end = end;
}

bool toml_fail_at(toml_ctx * ctx, size_t offset, GTEXT_TOML_Status code,
    const char * message) {
  if (!ctx->err) return false;
  /* A failure can be recorded before there is anything to point at: a NULL
   * buffer is refused as a parameter, and reporting that refusal used to walk
   * the buffer looking for the end of the line - which segfaulted on the one
   * input that can never be read. Found by the test that asked for the refusal
   * rather than by any document. */
  if (!ctx->buf) {
    ctx->err->code = code;
    ctx->err->message = message;
    ctx->err->offset = 0;
    ctx->err->line = 1;
    ctx->err->col = 1;
    return false;
  }
  /* First failure wins. A scanner that fails inside a scanner would otherwise
   * overwrite the position of the real problem with the position of its
   * consequence. */
  if (ctx->err->code != GTEXT_TOML_OK) return false;

  int line = 1;
  int col = 1;
  size_t line_start = 0;
  size_t line_end = 0;
  locate(ctx->buf, ctx->len, offset, &line, &col, &line_start, &line_end);

  ctx->err->code = code;
  ctx->err->message = message;
  ctx->err->offset = offset > ctx->len ? ctx->len : offset;
  ctx->err->line = line;
  ctx->err->col = col;

  size_t span = line_end - line_start;
  if (span > TOML_SNIPPET_MAX) span = TOML_SNIPPET_MAX;
  char * snippet = gtext_allocator_malloc(NULL, span + 1);
  if (snippet) {
    if (span) memcpy(snippet, ctx->buf + line_start, span);
    snippet[span] = '\0';
    ctx->err->context_snippet = snippet;
    ctx->err->context_snippet_len = span;
    size_t caret = ctx->err->offset - line_start;
    ctx->err->caret_offset = caret > span ? span : caret;
  }
  return false;
}

bool toml_fail(toml_ctx * ctx, GTEXT_TOML_Status code, const char * message) {
  return toml_fail_at(ctx, ctx->pos, code, message);
}
