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
 * Core CSV types and default initialization.
 */

#include <string.h>

#include <ghoti.io/text/macros.h>
#include "csv_internal.h"

#include <ghoti.io/text/csv/csv_core.h>
GTEXT_API GTEXT_CSV_Dialect gtext_csv_dialect_default(void) {
  GTEXT_CSV_Dialect d = {0};
  d.delimiter = ',';
  d.quote = '"';
  d.escape = GTEXT_CSV_ESCAPE_DOUBLED_QUOTE;
  d.newline_in_quotes = true;
  d.accept_lf = true;
  d.accept_crlf = true;
  d.accept_cr = false;
  d.trim_unquoted_fields = false;
  d.allow_space_after_delimiter = false;
  d.allow_unquoted_quotes = false;
  d.allow_unquoted_newlines = false;
  d.allow_comments = false;
  d.comment_prefix = "#";
  d.treat_first_row_as_header = false;
  d.header_dup_mode = GTEXT_CSV_DUPCOL_FIRST_WINS;
  return d;
}

/*
 * The presets are all gtext_csv_dialect_default() with the smallest change
 * that names the convention, so that a preset never quietly relaxes something
 * the caller did not ask about.  The one exception says so in its name.
 */

GTEXT_API GTEXT_CSV_Dialect gtext_csv_dialect_tsv(void) {
  GTEXT_CSV_Dialect d = gtext_csv_dialect_default();
  d.delimiter = '\t';
  return d;
}

GTEXT_API GTEXT_CSV_Dialect gtext_csv_dialect_semicolon(void) {
  GTEXT_CSV_Dialect d = gtext_csv_dialect_default();
  d.delimiter = ';';
  return d;
}

GTEXT_API GTEXT_CSV_Dialect gtext_csv_dialect_backslash_escape(void) {
  GTEXT_CSV_Dialect d = gtext_csv_dialect_default();
  d.escape = GTEXT_CSV_ESCAPE_BACKSLASH;
  return d;
}

GTEXT_API GTEXT_CSV_Dialect gtext_csv_dialect_excel(void) {
  /* RFC 4180 is Excel's format, so this is the default dialect.  It exists as
   * a name because callers coming from Python's csv module look for "excel",
   * and because a named constant is clearer at a call site than a comment
   * saying the default happens to match. */
  return gtext_csv_dialect_default();
}

GTEXT_API GTEXT_CSV_Dialect gtext_csv_dialect_permissive(void) {
  GTEXT_CSV_Dialect d = gtext_csv_dialect_default();
  d.trim_unquoted_fields = true;
  d.allow_space_after_delimiter = true;
  d.allow_unquoted_quotes = true;
  d.accept_cr = true;
  d.allow_comments = true;
  /* allow_unquoted_newlines is deliberately left off: its meaning is still
   * unsettled - the table parser disagrees with itself about a trailing CRLF
   * - so a preset must not turn it on.  See documentation/formats/csv.md. */
  return d;
}

GTEXT_API GTEXT_CSV_Parse_Options gtext_csv_parse_options_default(void) {
  GTEXT_CSV_Parse_Options opts = {0};
  opts.dialect = gtext_csv_dialect_default();
  opts.validate_utf8 = true;
  opts.in_situ_mode = false;
  opts.keep_bom = false;
  opts.max_rows = 0;         // Library default
  opts.max_cols = 0;         // Library default
  opts.max_field_bytes = 0;  // Library default
  opts.max_record_bytes = 0; // Library default
  opts.max_total_bytes = 0;  // Library default
  opts.enable_context_snippet = true;
  opts.context_radius_bytes = CSV_DEFAULT_CONTEXT_RADIUS_BYTES;
  return opts;
}

GTEXT_API GTEXT_CSV_Write_Options gtext_csv_write_options_default(void) {
  GTEXT_CSV_Write_Options opts = {0};
  opts.dialect = gtext_csv_dialect_default();
  opts.newline = "\n";
  opts.quote_all_fields = false;
  opts.quote_empty_fields = true;
  opts.quote_if_needed = true;
  opts.always_escape_quotes = true; // Default behavior depends on escape mode
  // True, which is what the writer has always done for a non-empty table:
  // the option was read by nothing, so every document got one regardless.
  // Defaulting to true keeps that output unchanged now that the option is
  // honored, rather than silently dropping a newline from every file the
  // library writes.
  opts.trailing_newline = true;
  opts.trim_trailing_empty_fields = false;
  /* The C library, which is what a write has always used. */
  opts.allocator = NULL;
  return opts;
}
