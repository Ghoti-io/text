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
 * Default initialization for JSON parse and write options.
 */

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/json/json_core.h>
GTEXT_API GTEXT_JSON_Parse_Options gtext_json_parse_options_default(void) {
  GTEXT_JSON_Parse_Options opts = {0};

  // Strictness / extensions - all off by default (strict JSON)
  opts.allow_comments = false;
  opts.allow_trailing_commas = false;
  opts.allow_nonfinite_numbers = false;
  opts.allow_single_quotes = false;
  opts.allow_unescaped_controls = false;
  opts.allow_hex_numbers = false;
  opts.allow_leading_plus = false;
  opts.allow_bare_decimal_point = false;
  opts.allow_ecma_escapes = false;
  opts.allow_line_continuations = false;
  opts.allow_ecma_whitespace = false;
  opts.allow_unquoted_keys = false;

  // Unicode / input handling
  opts.allow_leading_bom = true;  // default on
  opts.validate_utf8 = true;      // default on
  opts.normalize_unicode = false; // v2 feature, off by default
  opts.in_situ_mode = false;
  opts.allocator = NULL;      // off by default

  // Duplicate keys
  opts.dupkeys = GTEXT_JSON_DUPKEY_ERROR; // fail on duplicate keys

  // Limits - 0 means library default
  opts.max_depth = 0;
  opts.max_string_bytes = 0;
  opts.max_container_elems = 0;
  opts.max_total_bytes = 0;

  // Number fidelity / representations
  opts.preserve_number_lexeme = true; // preserve for round-trip correctness
  opts.parse_int64 = true;            // detect int64
  opts.parse_uint64 = true;           // detect uint64
  opts.parse_double = true;           // derive double

  return opts;
}

GTEXT_API GTEXT_JSON_Write_Options gtext_json_write_options_default(void) {
  GTEXT_JSON_Write_Options opts = {0};

  opts.allocator = NULL; // the default allocator; not the sink's buffer

  // Formatting
  opts.pretty = false;            // compact output
  opts.indent_spaces = 2;         // default indent (used if pretty = true)
  opts.newline = "\n";            // default newline
  opts.trailing_newline = false;  // no trailing newline by default
  opts.space_after_colon = false; // no space after colon by default
  opts.space_after_comma = false; // no space after comma by default
  opts.inline_array_threshold =
      -1; // always inline arrays by default (when not pretty)
  opts.inline_object_threshold =
      -1; // always inline objects by default (when not pretty)

  // Escaping
  opts.escape_solidus = false;       // don't escape forward slash by default
  opts.escape_unicode = false;       // don't escape non-ASCII by default
  opts.escape_all_non_ascii = false; // don't escape all non-ASCII

  // Canonical / deterministic
  opts.sort_object_keys = false;  // preserve insertion order
  opts.canonical_numbers = false; // preserve original lexeme

  // Extensions
  opts.allow_nonfinite_numbers = false; // don't emit nonfinite by default

  // Floating-point formatting
  opts.float_format =
      GTEXT_JSON_FLOAT_SHORTEST; // shortest representation by default
  opts.float_precision = 6; // default precision (used for FIXED/SCIENTIFIC)

  return opts;
}

GTEXT_API GTEXT_JSON_Parse_Options gtext_json_parse_options_json5(void) {
  /* Built from the default rather than from {0}, so a field added later starts
   * from the same place a strict parse would start from, and only the options
   * JSON5 names are changed here. */
  GTEXT_JSON_Parse_Options opts = gtext_json_parse_options_default();

  opts.allow_comments = true;
  opts.allow_trailing_commas = true;
  opts.allow_single_quotes = true;
  opts.allow_nonfinite_numbers = true;
  opts.allow_hex_numbers = true;
  opts.allow_leading_plus = true;
  opts.allow_bare_decimal_point = true;
  opts.allow_ecma_escapes = true;
  opts.allow_line_continuations = true;
  opts.allow_ecma_whitespace = true;
  opts.allow_unquoted_keys = true;

  /* Deliberately not touched: allow_unescaped_controls, because JSON5 permits
   * a raw control character in a string no more than JSON does; and
   * normalize_unicode, because JSON5 says nothing about normalization. The
   * limits and the duplicate-name policy are the default's. */
  return opts;
}
