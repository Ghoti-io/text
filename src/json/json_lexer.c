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
 * JSON lexer implementation.
 *
 * Tokenizes JSON input into tokens including punctuation, keywords,
 * strings, numbers, and comments (when enabled).
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "../idna/nfc_utf8_internal.h"
#include "json_internal.h"
#include "json_stream_internal.h"

#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/enums.h>

#include <ghoti.io/text/json/json_core.h>

/* ECMAScript's <USP> is General_Category=Zs, asked of ghoti.io-unicode rather
 * than of a table generated here. U+180E is why that matters: it was Zs until
 * Unicode 6.3 and is Cf now, so the answer is a property of the UCD version,
 * and there should be one UCD version in the suite rather than one per
 * library. */
static int json5_space_table_has(uint32_t cp) {
  return guni_general_category(cp) == GUNI_GC_ZS;
}

/* How many bytes the UTF-8 character starting with `lead` occupies, or 0 if
 * that byte cannot start one. */
static size_t json_utf8_lead_length(unsigned char lead) {
  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    return 2;
  }
  if ((lead & 0xF0) == 0xE0) {
    return 3;
  }
  if ((lead & 0xF8) == 0xF0) {
    return 4;
  }
  return 0;
}

/* Decode one UTF-8 character. Returns its length in bytes and writes the
 * codepoint, or 0 if the bytes are not a well-formed character - which here
 * means "not whitespace" rather than an error, because a malformed sequence
 * between tokens is for the token that follows to complain about. */
static size_t json_utf8_decode(
    const char * p, size_t available, uint32_t * out_cp) {
  const unsigned char * u = (const unsigned char *)p;
  if (available == 0) {
    return 0;
  }
  if (u[0] < 0x80) {
    *out_cp = u[0];
    return 1;
  }
  size_t len;
  uint32_t cp;
  if ((u[0] & 0xE0) == 0xC0) {
    len = 2;
    cp = u[0] & 0x1Fu;
  }
  else if ((u[0] & 0xF0) == 0xE0) {
    len = 3;
    cp = u[0] & 0x0Fu;
  }
  else if ((u[0] & 0xF8) == 0xF0) {
    len = 4;
    cp = u[0] & 0x07u;
  }
  else {
    return 0;
  }
  if (available < len) {
    return 0;
  }
  for (size_t i = 1; i < len; i++) {
    if ((u[i] & 0xC0) != 0x80) {
      return 0;
    }
    cp = (cp << 6) | (u[i] & 0x3Fu);
  }
  *out_cp = cp;
  return len;
}

/* One hex digit's value, or -1. json_string.c has its own copy of this for
 * the escape decoder; a shared one would mean a header for four lines. */
static int json_hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}


/* May this codepoint start an IdentifierName, or continue one?
 *
 * ID_Start and ID_Continue answer most of it; `$` and `_` are ECMAScript's own
 * additions, and `_` needs naming only as a *start* because the UCD has it as
 * Pc and therefore already as a continuation. ZWNJ and ZWJ need no naming at
 * all: they carry ID_Continue in this UCD, so the property admits them exactly
 * where ECMAScript does.
 *
 * The properties come from ghoti.io-unicode. They used to come from a table
 * generated here, which is the same data regex generated separately and font
 * would have generated a third time. */
static int json5_ident_start(uint32_t cp) {
  return cp == '$' || cp == '_'
      || guni_has_property(cp, GUNI_PROP_ID_START);
}

static int json5_ident_continue(uint32_t cp) {
  return cp == '$' || guni_has_property(cp, GUNI_PROP_ID_CONTINUE);
}

/* The whitespace at `p`, in bytes, or 0 if there is none. `*ends_line` says
 * whether it ends a line, which is what the position counter needs.
 *
 * JSON allows four characters here. ECMAScript's WhiteSpace and
 * LineTerminator productions, which JSON5 takes, add vertical tab, form feed,
 * U+FEFF, every Zs, and U+2028 and U+2029. Zs is a Unicode property and
 * is asked of ghoti.io-unicode; the others are named by ECMAScript rather than
 * by a property, so they are written out. */
static size_t json_lexer_whitespace_length(
    const json_lexer * lexer, const char * p, size_t available,
    int * ends_line) {
  *ends_line = 0;
  if (available == 0) {
    return 0;
  }
  const unsigned char c = (unsigned char)p[0];
  if (c == '\n') {
    *ends_line = 1;
    return 1;
  }
  if (c == ' ' || c == '\t' || c == '\r') {
    return 1;
  }
  if (!(lexer->opts && lexer->opts->allow_ecma_whitespace)) {
    return 0;
  }
  if (c == 0x0B || c == 0x0C) { // Vertical tab, form feed
    return 1;
  }
  if (c < 0x80) {
    return 0;
  }
  uint32_t cp;
  const size_t len = json_utf8_decode(p, available, &cp);
  if (len == 0) {
    return 0;
  }
  if (cp == 0x2028 || cp == 0x2029) { // LINE and PARAGRAPH SEPARATOR
    *ends_line = 1;
    return len;
  }
  if (cp == 0xFEFF) { // ZERO WIDTH NO-BREAK SPACE
    return len;
  }
  return json5_space_table_has(cp) ? len : 0;
}

// Skip whitespace characters
static void json_lexer_skip_whitespace(json_lexer * lexer) {
  while (lexer->current_offset < lexer->input_len) {
    // Defensive bounds check before buffer access
    if (!json_check_bounds_offset(lexer->current_offset, lexer->input_len)) {
      break;
    }
    int ends_line = 0;
    const size_t len = json_lexer_whitespace_length(lexer,
        lexer->input + lexer->current_offset,
        lexer->input_len - lexer->current_offset, &ends_line);
    if (len == 0) {
      break;
    }
    if (ends_line) {
      json_position_increment_line(&lexer->pos);
      lexer->pos.col = 1;
    }
    else {
      json_position_update_column(&lexer->pos, 1);
    }
    // Check for overflow before incrementing
    if (json_check_add_overflow(lexer->current_offset, len)) {
      // Overflow - saturate at SIZE_MAX
      lexer->current_offset = SIZE_MAX;
      json_position_update_offset(&lexer->pos, len);
      break;
    }
    lexer->current_offset += len;
    json_position_update_offset(&lexer->pos, len);
  }
}

/* Skip a single-line comment (//).
 *
 * Returns 1 if a comment was skipped, 0 if there is not one here, and -2 if
 * one began and the buffer ended before its newline did while more input may
 * still arrive. That third answer is the streaming case, and it matters: a
 * comment running to the end of a chunk is *not* a comment that ended, and
 * treating it as one reads the rest of it, in the next chunk, as code. */
static int json_lexer_skip_single_line_comment(json_lexer * lexer) {
  // Check for underflow and sufficient length using shared helper
  if (lexer->input_len < 2 || json_check_sub_underflow(lexer->input_len, 2) ||
      lexer->current_offset > lexer->input_len - 2) {
    return 0;
  }
  // Defensive bounds checks before buffer access
  if (!json_check_bounds_offset(lexer->current_offset, lexer->input_len) ||
      !json_check_bounds_offset(lexer->current_offset + 1, lexer->input_len)) {
    return 0;
  }
  if (lexer->input[lexer->current_offset] == '/' &&
      lexer->input[lexer->current_offset + 1] == '/') {
    // Skip to end of line or end of input
    while (lexer->current_offset < lexer->input_len) {
      // Defensive bounds check before buffer access
      if (!json_check_bounds_offset(lexer->current_offset, lexer->input_len)) {
        break;
      }
      if (lexer->input[lexer->current_offset] == '\n') {
        json_position_increment_line(&lexer->pos);
        lexer->pos.col = 1;
        // Check for overflow before incrementing
        if (json_check_add_overflow(lexer->current_offset, 1)) {
          // Overflow - saturate at SIZE_MAX
          lexer->current_offset = SIZE_MAX;
          json_position_update_offset(&lexer->pos, 1);
          return 1;
        }
        lexer->current_offset++;
        json_position_update_offset(&lexer->pos, 1);
        return 1;
      }
      // Check for overflow before incrementing
      if (json_check_add_overflow(lexer->current_offset, 1)) {
        // Overflow - saturate at SIZE_MAX and break
        lexer->current_offset = SIZE_MAX;
        break;
      }
      lexer->current_offset++;
      json_position_update_offset(&lexer->pos, 1);
    }
    /* The buffer ended with no newline. In a complete document that is a
     * comment ended by the end of input, which is allowed; in a stream it is
     * a comment that has not finished arriving. */
    if (lexer->streaming_mode) {
      return -2;
    }
    // Update position offset to match current_offset
    lexer->pos.offset = lexer->current_offset;
    return 1;
  }
  return 0;
}

// Skip a multi-line comment of the form /* ... */. Returns 1 if skipped, 0 if
// not a comment, -1 on error (unclosed comment).
static int json_lexer_skip_multi_line_comment(json_lexer * lexer) {
  // Check for underflow and sufficient length using shared helper
  if (lexer->input_len < 2 || json_check_sub_underflow(lexer->input_len, 2) ||
      lexer->current_offset > lexer->input_len - 2) {
    return 0;
  }
  // Defensive bounds checks before buffer access
  if (!json_check_bounds_offset(lexer->current_offset, lexer->input_len) ||
      !json_check_bounds_offset(lexer->current_offset + 1, lexer->input_len)) {
    return 0;
  }
  if (lexer->input[lexer->current_offset] == '/' &&
      lexer->input[lexer->current_offset + 1] == '*') {
    // Skip /* and look for */
    // Check for overflow before adding 2
    if (json_check_add_overflow(lexer->current_offset, 2)) {
      // Overflow - saturate at SIZE_MAX
      lexer->current_offset = SIZE_MAX;
      return -1; // Error: unclosed comment
    }
    lexer->current_offset += 2;
    json_position_update_offset(&lexer->pos, 2);
    // Check for underflow: current_offset + 1 could overflow
    while (lexer->current_offset < lexer->input_len) {
      // Defensive bounds check before buffer access
      if (!json_check_bounds_offset(lexer->current_offset, lexer->input_len)) {
        break;
      }
      // Check if we have at least 2 bytes remaining
      if (json_check_sub_underflow(lexer->input_len, lexer->current_offset) ||
          lexer->input_len - lexer->current_offset < 2) {
        break; // Not enough bytes for "*/"
      }
      // Defensive bounds check for current_offset + 1
      if (!json_check_bounds_offset(
              lexer->current_offset + 1, lexer->input_len)) {
        break;
      }
      if (lexer->input[lexer->current_offset] == '*' &&
          lexer->input[lexer->current_offset + 1] == '/') {
        // Found closing */
        // Check for overflow before adding 2
        if (json_check_add_overflow(lexer->current_offset, 2)) {
          // Overflow - saturate at SIZE_MAX
          lexer->current_offset = SIZE_MAX;
          json_position_update_offset(&lexer->pos, 2);
          return 1;
        }
        lexer->current_offset += 2;
        json_position_update_offset(&lexer->pos, 2);
        // Update line/col if needed (we don't track precisely in comments)
        return 1;
      }
      // Defensive bounds check before buffer access
      if (!json_check_bounds_offset(lexer->current_offset, lexer->input_len)) {
        break;
      }
      if (lexer->input[lexer->current_offset] == '\n') {
        json_position_increment_line(&lexer->pos);
        lexer->pos.col = 1;
      }
      else {
        json_position_update_column(&lexer->pos, 1);
      }
      // Check for overflow before incrementing
      if (json_check_add_overflow(lexer->current_offset, 1)) {
        // Overflow - saturate at SIZE_MAX and break
        lexer->current_offset = SIZE_MAX;
        break;
      }
      lexer->current_offset++;
      json_position_update_offset(&lexer->pos, 1);
    }
    /* The closing delimiter is not in the buffer. In a complete document the
     * comment is
     * unclosed and that is an error; in a stream it may still be coming, and it
     * may even be split - a star at the end of one chunk and a slash at the
     * start of the next, which is why the loop above stops two bytes short
     * rather than reading the star as ordinary text. */
    if (lexer->streaming_mode) {
      return -2;
    }
    // Unclosed comment
    return -1;
  }
  return 0;
}

// Skip comments if enabled
static GTEXT_JSON_Status json_lexer_skip_comments(json_lexer * lexer) {
  if (!lexer->opts || !lexer->opts->allow_comments) {
    return GTEXT_JSON_OK;
  }

  int skipped;
  do {
    skipped = 0;
    /* Where a comment is unfinished, everything about this call is undone: the
     * caller keeps the bytes and lexes them again when the rest arrives, so
     * the offset and the position have to be where they were. Without the
     * rewind the comment's first half would be consumed and its second half
     * read as code. */
    const size_t restore_offset = lexer->current_offset;
    const json_position restore_pos = lexer->pos;

    // Try single-line comment
    const int single_result = json_lexer_skip_single_line_comment(lexer);
    if (single_result == 1) {
      skipped = 1;
    }
    else if (single_result == -2) {
      lexer->current_offset = restore_offset;
      lexer->pos = restore_pos;
      return GTEXT_JSON_E_INCOMPLETE;
    }
    // Try multi-line comment
    int multi_result = json_lexer_skip_multi_line_comment(lexer);
    if (multi_result == 1) {
      skipped = 1;
    }
    else if (multi_result == -1) {
      return GTEXT_JSON_E_BAD_TOKEN; // Unclosed comment
    }
    else if (multi_result == -2) {
      lexer->current_offset = restore_offset;
      lexer->pos = restore_pos;
      return GTEXT_JSON_E_INCOMPLETE;
    }
    // Skip whitespace after comments
    if (skipped) {
      json_lexer_skip_whitespace(lexer);
    }
  } while (skipped);

  /* A lone slash at the end of the buffer begins a comment whose second
   * character has not arrived: the two comment openings are the only tokens
   * that start with one, so it is unfinished rather than unknown. */
  if (lexer->streaming_mode && lexer->current_offset < lexer->input_len &&
      lexer->input_len - lexer->current_offset == 1 &&
      lexer->input[lexer->current_offset] == '/') {
    return GTEXT_JSON_E_INCOMPLETE;
  }

  return GTEXT_JSON_OK;
}

// Check if character is a valid identifier start (for keywords)
static int json_is_identifier_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Check if character is a valid identifier continuation
static int json_is_identifier_cont(char c) {
  return json_is_identifier_start(c) || (c >= '0' && c <= '9');
}

// Check if a partial identifier could be a prefix of a JSON keyword
// Returns 1 if it's a valid prefix, 0 if not
// Note: This function doesn't check for NaN/Infinity prefixes because those
// are extension keywords that depend on allow_nonfinite_numbers option.
// The lexer handles those separately in json_lexer_match_keyword().
static int json_is_keyword_prefix(const char * str, size_t len) {
  if (len == 0) {
    return 0;
  }

  // Check prefixes of standard keywords
  if (str[0] == 't') {
    // Could be "true"
    if (len == 1)
      return 1; // "t"
    if (len == 2 && str[1] == 'r')
      return 1; // "tr"
    if (len == 3 && str[1] == 'r' && str[2] == 'u')
      return 1; // "tru"
    if (len == 4 && str[1] == 'r' && str[2] == 'u' && str[3] == 'e')
      return 1; // "true" (complete)
    return 0;
  }
  if (str[0] == 'f') {
    // Could be "false"
    if (len == 1)
      return 1; // "f"
    if (len == 2 && str[1] == 'a')
      return 1; // "fa"
    if (len == 3 && str[1] == 'a' && str[2] == 'l')
      return 1; // "fal"
    if (len == 4 && str[1] == 'a' && str[2] == 'l' && str[3] == 's')
      return 1; // "fals"
    if (len == 5 && str[1] == 'a' && str[2] == 'l' && str[3] == 's' &&
        str[4] == 'e')
      return 1; // "false" (complete)
    return 0;
  }
  if (str[0] == 'n') {
    // Could be "null"
    if (len == 1)
      return 1; // "n"
    if (len == 2 && str[1] == 'u')
      return 1; // "nu"
    if (len == 3 && str[1] == 'u' && str[2] == 'l')
      return 1; // "nul"
    if (len == 4 && str[1] == 'u' && str[2] == 'l' && str[3] == 'l')
      return 1; // "null" (complete)
    return 0;
  }
  if (str[0] == 'N') {
    // Could be "NaN" (if extensions enabled)
    if (len == 1)
      return 1; // "N"
    if (len == 2 && str[1] == 'a')
      return 1; // "Na"
    if (len == 3 && str[1] == 'a' && str[2] == 'N')
      return 1; // "NaN" (complete)
    return 0;
  }
  if (str[0] == 'I') {
    // Could be "Infinity" (if extensions enabled)
    if (len == 1)
      return 1; // "I"
    if (len == 2 && str[1] == 'n')
      return 1; // "In"
    if (len == 3 && str[1] == 'n' && str[2] == 'f')
      return 1; // "Inf"
    if (len == 4 && str[1] == 'n' && str[2] == 'f' && str[3] == 'i')
      return 1; // "Infi"
    if (len == 5 && str[1] == 'n' && str[2] == 'f' && str[3] == 'i' &&
        str[4] == 'n')
      return 1; // "Infin"
    if (len == 6 && str[1] == 'n' && str[2] == 'f' && str[3] == 'i' &&
        str[4] == 'n' && str[5] == 'i')
      return 1; // "Infini"
    if (len == 7 && str[1] == 'n' && str[2] == 'f' && str[3] == 'i' &&
        str[4] == 'n' && str[5] == 'i' && str[6] == 't')
      return 1; // "Infinit"
    if (len == 8 && str[1] == 'n' && str[2] == 'f' && str[3] == 'i' &&
        str[4] == 'n' && str[5] == 'i' && str[6] == 't' && str[7] == 'y')
      return 1; // "Infinity" (complete)
    return 0;
  }

  return 0;
}

// Try to match a keyword or extension token. Returns 1 if matched, 0 if not.
static int json_lexer_match_keyword(json_lexer * lexer, json_token * token) {
  size_t start = lexer->current_offset;
  size_t len = 0;

  // Read identifier
  // Defensive bounds check before buffer access
  if (start >= lexer->input_len ||
      !json_check_bounds_offset(start, lexer->input_len)) {
    return 0;
  }
  if (!json_is_identifier_start(lexer->input[start])) {
    return 0;
  }

  // Check for underflow in subtraction and overflow in addition
  while (len < lexer->input_len) {
    // Check for underflow: input_len - start
    if (json_check_sub_underflow(lexer->input_len, start) ||
        len >= lexer->input_len - start) {
      break;
    }
    // Check for overflow: start + len
    if (json_check_add_overflow(start, len)) {
      break;
    }
    // Defensive bounds check before buffer access
    if (!json_check_bounds_offset(start + len, lexer->input_len)) {
      break;
    }
    if (!json_is_identifier_cont(lexer->input[start + len])) {
      break;
    }
    // Check for overflow before incrementing len
    if (json_check_add_overflow(len, 1)) {
      break;
    }
    len++;
  }

  if (len == 0) {
    return 0;
  }

  /* In streaming mode a run that reaches the end of what has arrived is
   * unfinished rather than a keyword. The word decided here is only the word
   * if nothing follows it that would make it longer: `null` with a chunk
   * boundary behind it may be the first four bytes of the name `nullzc`, and
   * committing the keyword spends the bytes on a token the next chunk
   * contradicts. Nothing is consumed - the caller keeps the bytes and lexes
   * them again when more arrive - and gtext_json_stream_finish() clears
   * streaming_mode, so the last keyword in a document is not left waiting.
   *
   * This is the rule json_lexer_parse_identifier() already applies to the
   * same bytes. The keyword path needs it too, because it is reached first
   * and returns without ever giving the identifier path a look. */
  if (lexer->streaming_mode && len >= lexer->input_len - start) {
    return GTEXT_JSON_E_INCOMPLETE;
  }

  const char * keyword_start = lexer->input + start;

  // Check for standard keywords
  if (json_matches(keyword_start, len, "null")) {
    token->type = JSON_TOKEN_NULL;
    token->pos = lexer->pos;
    token->length = len;
    // Check for overflow before adding start + len
    if (json_check_add_overflow(start, len)) {
      // Overflow - saturate at SIZE_MAX
      lexer->current_offset = SIZE_MAX;
      lexer->pos.offset = SIZE_MAX;
    }
    else {
      lexer->current_offset = start + len;
      lexer->pos.offset = lexer->current_offset;
    }
    json_position_update_column(&lexer->pos, len);
    return 1;
  }
  if (json_matches(keyword_start, len, "true")) {
    token->type = JSON_TOKEN_TRUE;
    token->pos = lexer->pos;
    token->length = len;
    // Check for overflow before adding start + len
    if (json_check_add_overflow(start, len)) {
      // Overflow - saturate at SIZE_MAX
      lexer->current_offset = SIZE_MAX;
      lexer->pos.offset = SIZE_MAX;
    }
    else {
      lexer->current_offset = start + len;
      lexer->pos.offset = lexer->current_offset;
    }
    json_position_update_column(&lexer->pos, len);
    return 1;
  }
  if (json_matches(keyword_start, len, "false")) {
    token->type = JSON_TOKEN_FALSE;
    token->pos = lexer->pos;
    token->length = len;
    // Check for overflow before adding start + len
    if (json_check_add_overflow(start, len)) {
      // Overflow - saturate at SIZE_MAX
      lexer->current_offset = SIZE_MAX;
      lexer->pos.offset = SIZE_MAX;
    }
    else {
      lexer->current_offset = start + len;
      lexer->pos.offset = lexer->current_offset;
    }
    json_position_update_column(&lexer->pos, len);
    return 1;
  }

  // Check for extension tokens (NaN, Infinity)
  // Note: "Infinity" is checked here because it starts with an identifier
  // character.
  // "-Infinity" is checked separately in the number parsing path (see
  // json_lexer_match_neg_infinity()) because it starts with '-'.
  // Always check for these tokens, but return error if not allowed
  if (json_matches(keyword_start, len, "NaN")) {
    if (lexer->opts && lexer->opts->allow_nonfinite_numbers) {
      token->type = JSON_TOKEN_NAN;
      token->pos = lexer->pos;
      token->length = len;
      // Check for overflow before adding start + len
      if (json_check_add_overflow(start, len)) {
        // Overflow - saturate at SIZE_MAX
        lexer->current_offset = SIZE_MAX;
        lexer->pos.offset = SIZE_MAX;
      }
      else {
        lexer->current_offset = start + len;
        lexer->pos.offset = lexer->current_offset;
      }
      json_position_update_column(&lexer->pos, len);
      return 1;
    }
    else {
      // NaN not allowed - return error
      token->type = JSON_TOKEN_ERROR;
      token->pos = lexer->pos;
      token->length = len;
      // Check for overflow before adding start + len
      if (json_check_add_overflow(start, len)) {
        // Overflow - saturate at SIZE_MAX
        lexer->current_offset = SIZE_MAX;
        lexer->pos.offset = SIZE_MAX;
      }
      else {
        lexer->current_offset = start + len;
        lexer->pos.offset = lexer->current_offset;
      }
      json_position_update_column(&lexer->pos, len);
      return GTEXT_JSON_E_NONFINITE;
    }
  }
  if (json_matches(keyword_start, len, "Infinity")) {
    if (lexer->opts && lexer->opts->allow_nonfinite_numbers) {
      token->type = JSON_TOKEN_INFINITY;
      token->pos = lexer->pos;
      token->length = len;
      // Check for overflow before adding start + len
      if (json_check_add_overflow(start, len)) {
        // Overflow - saturate at SIZE_MAX
        lexer->current_offset = SIZE_MAX;
        lexer->pos.offset = SIZE_MAX;
      }
      else {
        lexer->current_offset = start + len;
        lexer->pos.offset = lexer->current_offset;
      }
      json_position_update_column(&lexer->pos, len);
      return 1;
    }
    else {
      // Infinity not allowed - return error
      token->type = JSON_TOKEN_ERROR;
      token->pos = lexer->pos;
      token->length = len;
      // Check for overflow before adding start + len
      if (json_check_add_overflow(start, len)) {
        // Overflow - saturate at SIZE_MAX
        lexer->current_offset = SIZE_MAX;
        lexer->pos.offset = SIZE_MAX;
      }
      else {
        lexer->current_offset = start + len;
        lexer->pos.offset = lexer->current_offset;
      }
      json_position_update_column(&lexer->pos, len);
      return GTEXT_JSON_E_NONFINITE;
    }
  }

  return 0;
}

// Try to match -Infinity (special case, starts with -).
// This is separate from the "Infinity" check in json_lexer_match_keyword()
// because "-Infinity" starts with a minus sign, which is not an identifier
// character. Therefore it cannot be matched by the keyword matcher and must
// be handled in the number parsing path.
// Returns 1 if matched and allowed, GTEXT_JSON_E_NONFINITE if matched but not
// allowed, 0 if not matched.
/* How a run of bytes that starts with a sign relates to the signed nonfinite
 * spellings: 2 if it is exactly one of them, 1 if it is a proper prefix of
 * one, 0 if it is neither. The streaming lexer needs all three answers, and
 * needs them for "NaN" as well as "Infinity" - the unsigned words go through
 * the keyword path, which has json_is_keyword_prefix() for the same job, but
 * anything with a sign arrives in the number path instead. */
static int json_signed_nonfinite_match(
    const char * content, size_t len, int allow_plus) {
  static const char * const words[] = {
      "-Infinity", "+Infinity", "-NaN", "+NaN"};
  int best = 0;
  if (len == 0) {
    return 0;
  }
  for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
    const char * word = words[w];
    if (word[0] == '+' && !allow_plus) {
      continue;
    }
    const size_t word_len = strlen(word);
    if (len > word_len || memcmp(content, word, len) != 0) {
      continue;
    }
    if (len == word_len) {
      return 2;
    }
    best = 1;
  }
  return best;
}

/* A nonfinite value with a sign in front of it: "-Infinity" as JSON's
 * extensions have always spelled it, and "+Infinity", "-NaN" and "+NaN" as
 * JSON5 does. The sign is what brings these here rather than to the keyword
 * path: '-' and '+' also start numbers.
 *
 * Returns 1 on a match, GTEXT_JSON_E_NONFINITE for a spelling this parser
 * recognises but was not asked to accept, and 0 for anything else - which
 * leaves the caller to read it as a number.
 *
 * NaN has no meaningful sign, so "-NaN" and "+NaN" both produce
 * JSON_TOKEN_NAN. Infinity's sign decides between two token types.
 */
static int json_lexer_match_signed_nonfinite(
    json_lexer * lexer, json_token * token) {
  const size_t start = lexer->current_offset;
  if (!json_check_bounds_offset(start, lexer->input_len)) {
    return 0;
  }
  const char sign = lexer->input[start];
  if (sign != '-' && sign != '+') {
    return 0;
  }
  /* A leading '+' is JSON5's alone. Without that option "+Infinity" is not a
   * spelling this parser knows, so it must read as a bad number rather than
   * as a refused nonfinite. */
  if (sign == '+' && !(lexer->opts && lexer->opts->allow_leading_plus)) {
    return 0;
  }

  json_token_type matched;
  size_t length;
  if (lexer->input_len - start >= 9 &&
      json_matches(lexer->input + start + 1, 8, "Infinity")) {
    matched = sign == '-' ? JSON_TOKEN_NEG_INFINITY : JSON_TOKEN_INFINITY;
    length = 9;
  }
  else if (lexer->input_len - start >= 4 &&
      json_matches(lexer->input + start + 1, 3, "NaN")) {
    matched = JSON_TOKEN_NAN;
    length = 4;
  }
  else {
    return 0;
  }

  const int allowed = lexer->opts && lexer->opts->allow_nonfinite_numbers;
  token->type = allowed ? matched : JSON_TOKEN_ERROR;
  token->pos = lexer->pos;
  token->length = length;
  if (json_check_add_overflow(start, length)) {
    lexer->current_offset = SIZE_MAX;
    lexer->pos.offset = SIZE_MAX;
  }
  else {
    lexer->current_offset = start + length;
    lexer->pos.offset = lexer->current_offset;
  }
  json_position_update_column(&lexer->pos, length);
  return allowed ? 1 : GTEXT_JSON_E_NONFINITE;
}

/* One character of an IdentifierName at `p`: its codepoint, and how many bytes
 * of input it occupies. A `\uXXXX` escape is one character six bytes long, and
 * a surrogate pair is one character twelve bytes long - ECMAScript 5.1 writes
 * an astral character that way, and JSON5 is written against 5.1.
 *
 * Returns JSON5_IDENT_CHAR_OK, JSON5_IDENT_CHAR_NONE when the bytes are not
 * part of a name at all, or JSON5_IDENT_CHAR_TRUNCATED when the input ends in
 * the middle of an escape - which is a different answer because more input can
 * still change it. */
typedef enum {
  JSON5_IDENT_CHAR_OK = 0,
  JSON5_IDENT_CHAR_NONE = 1,
  JSON5_IDENT_CHAR_TRUNCATED = 2
} json5_ident_char_status;

static json5_ident_char_status json5_ident_char(
    const char * p, size_t available, uint32_t * out_cp, size_t * out_len) {
  if (available == 0) {
    return JSON5_IDENT_CHAR_NONE;
  }
  if (p[0] != '\\') {
    const size_t len = json_utf8_decode(p, available, out_cp);
    if (len == 0) {
      /* A truncated character is not the same as a character that cannot be in
       * a name: the lead byte says how many bytes it wanted. */
      const size_t want = json_utf8_lead_length((unsigned char)p[0]);
      if (want > available) {
        return JSON5_IDENT_CHAR_TRUNCATED;
      }
      return JSON5_IDENT_CHAR_NONE;
    }
    *out_len = len;
    return JSON5_IDENT_CHAR_OK;
  }

  /* An escape. Only \uXXXX: \u{...} is ECMAScript 2015's and the JSON5
   * specification is written against 5.1. */
  if (available < 2) {
    return JSON5_IDENT_CHAR_TRUNCATED;
  }
  if (p[1] != 'u') {
    return JSON5_IDENT_CHAR_NONE;
  }
  if (available < 6) {
    return JSON5_IDENT_CHAR_TRUNCATED;
  }
  uint32_t cp = 0;
  for (size_t i = 2; i < 6; i++) {
    const int digit = json_hex_value(p[i]);
    if (digit < 0) {
      return JSON5_IDENT_CHAR_NONE;
    }
    cp = (cp << 4) | (uint32_t)digit;
  }
  if (cp >= 0xD800 && cp <= 0xDBFF) {
    /* A high surrogate names half a character. The other half has to be a
     * second escape, as it does in ECMAScript. */
    if (available < 8) {
      return JSON5_IDENT_CHAR_TRUNCATED;
    }
    if (p[6] != '\\' || p[7] != 'u') {
      return JSON5_IDENT_CHAR_NONE;
    }
    if (available < 12) {
      return JSON5_IDENT_CHAR_TRUNCATED;
    }
    uint32_t low = 0;
    for (size_t i = 8; i < 12; i++) {
      const int digit = json_hex_value(p[i]);
      if (digit < 0) {
        return JSON5_IDENT_CHAR_NONE;
      }
      low = (low << 4) | (uint32_t)digit;
    }
    if (low < 0xDC00 || low > 0xDFFF) {
      return JSON5_IDENT_CHAR_NONE;
    }
    *out_cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
    *out_len = 12;
    return JSON5_IDENT_CHAR_OK;
  }
  if (cp >= 0xDC00 && cp <= 0xDFFF) {
    return JSON5_IDENT_CHAR_NONE; // A low surrogate on its own.
  }
  *out_cp = cp;
  *out_len = 6;
  return JSON5_IDENT_CHAR_OK;
}

/* Write `cp` as UTF-8 at `out`, returning the number of bytes. */
static size_t json5_utf8_encode(uint32_t cp, char * out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* An unquoted object name: JSON5's JSON5Identifier, which is ECMAScript's
 * IdentifierName.
 *
 * Returns 1 for a name, 0 if the input does not begin one - the caller then
 * reports whatever it would have reported anyway - and a status otherwise.
 * Two passes: the first measures the run, so the buffer for the decoded name
 * can be the run's length rather than the rest of the document. Escapes only
 * ever shrink, six bytes to at most four.
 *
 * In streaming mode a run that reaches the end of the buffer is unfinished
 * rather than complete, and nothing is consumed - the same arrangement the
 * keyword path uses, and it works for the same reason: the caller keeps the
 * bytes and lexes them again when more arrive. gtext_json_stream_finish()
 * clears streaming_mode, so the last name in a document is not left waiting.
 */
static int json_lexer_parse_identifier(
    json_lexer * lexer, json_token * token, GTEXT_JSON_Status * out_status) {
  const size_t start = lexer->current_offset;
  size_t at = start;
  size_t characters = 0;

  while (at < lexer->input_len) {
    uint32_t cp = 0;
    size_t len = 0;
    const json5_ident_char_status got = json5_ident_char(
        lexer->input + at, lexer->input_len - at, &cp, &len);
    if (got == JSON5_IDENT_CHAR_TRUNCATED) {
      if (lexer->streaming_mode) {
        *out_status = GTEXT_JSON_E_INCOMPLETE;
        return 1;
      }
      break;
    }
    if (got == JSON5_IDENT_CHAR_NONE) {
      break;
    }
    if (!(characters == 0 ? json5_ident_start(cp) : json5_ident_continue(cp))) {
      break;
    }
    at += len;
    characters++;
  }

  if (characters == 0) {
    return 0; // Not a name.
  }
  if (lexer->streaming_mode && at >= lexer->input_len) {
    /* The run reached the end of what has arrived, so the next chunk may
     * continue it. Nothing is consumed. */
    *out_status = GTEXT_JSON_E_INCOMPLETE;
    return 1;
  }

  const size_t run_len = at - start;
  char * decoded = (char *)gtext_allocator_malloc(
      lexer->opts ? lexer->opts->allocator : NULL, run_len + 1);
  if (!decoded) {
    *out_status = GTEXT_JSON_E_OOM;
    return 1;
  }

  size_t out_len = 0;
  size_t scan = start;
  while (scan < at) {
    uint32_t cp = 0;
    size_t len = 0;
    if (json5_ident_char(lexer->input + scan, at - scan, &cp, &len) !=
        JSON5_IDENT_CHAR_OK) {
      break; // Cannot happen: the first pass accepted these bytes.
    }
    out_len += json5_utf8_encode(cp, decoded + out_len);
    scan += len;
  }
  decoded[out_len] = '\0';

  /* NFC, if it was asked for. A name is a name however it was written, so an
   * unquoted one is normalised exactly as a quoted one is - otherwise
   * duplicate-name detection would answer differently for `{"caf\u00e9":1,
   * cafe\u0301:2}` than for the same pair both quoted. */
  if (lexer->opts && lexer->opts->normalize_unicode) {
    char * normalized = NULL;
    size_t normalized_len = 0;
    const int nfc = gtext_nfc_utf8(
        lexer->opts->allocator, decoded, out_len, &normalized, &normalized_len);
    gtext_allocator_free(lexer->opts->allocator, decoded);
    if (nfc != 1) {
      *out_status = nfc == 0 ? GTEXT_JSON_E_BAD_UNICODE : GTEXT_JSON_E_OOM;
      return 1;
    }
    decoded = normalized;
    out_len = normalized_len;
  }

  token->type = JSON_TOKEN_IDENT;
  token->pos = lexer->pos;
  token->length = run_len;
  token->data.string.alloc = lexer->opts ? lexer->opts->allocator : NULL;
  token->data.string.value = decoded;
  token->data.string.value_len = out_len;
  token->data.string.original_start = start;
  token->data.string.original_len = run_len;

  lexer->current_offset = at;
  lexer->pos.offset = at;
  json_position_update_column(&lexer->pos, characters);
  *out_status = GTEXT_JSON_OK;
  return 1;
}

// Parse a string token
static GTEXT_JSON_Status json_lexer_parse_string(
    json_lexer * lexer, json_token * token) {
  json_token_buffer * tb = lexer->token_buffer;
  size_t start = lexer->current_offset;
  char quote_char;
  int resuming = 0;

  // Check if resuming from incomplete string
  if (tb && tb->type == JSON_TOKEN_BUFFER_STRING) {
    resuming = 1;
    // We're resuming - the opening quote and partial content are already in the
    // buffer Determine quote character from buffered data
    if (tb->buffer_used > 0 && tb->buffer[0] == '"') {
      quote_char = '"';
    }
    else if (tb->buffer_used > 0 && tb->buffer[0] == '\'') {
      quote_char = '\'';
    }
    else {
      // Invalid state - buffer should have opening quote
      json_token_buffer_clear(tb);
      return GTEXT_JSON_E_BAD_TOKEN;
    }
    // When resuming, we continue parsing from current_offset
    // The buffer already has opening quote and previous content, we'll append
    // new content string_start tracks where we start in the current input chunk
  }
  else {
    // Starting new string
    // Defensive bounds check before buffer access
    if (start >= lexer->input_len ||
        !json_check_bounds_offset(start, lexer->input_len)) {
      return GTEXT_JSON_E_BAD_TOKEN;
    }
    quote_char = lexer->input[start];

    // Check for single quotes (if enabled)
    int allow_single = lexer->opts && lexer->opts->allow_single_quotes;
    if (quote_char != '"' && (!allow_single || quote_char != '\'')) {
      return GTEXT_JSON_E_BAD_TOKEN;
    }

    // Initialize token buffer if available
    if (tb) {
      json_token_buffer_clear(tb);
      tb->type = JSON_TOKEN_BUFFER_STRING;
      tb->start_offset = start;
      // Append opening quote to buffer
      GTEXT_JSON_Status status = json_token_buffer_append(tb, &quote_char, 1);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    // Check for overflow before incrementing start
    if (json_check_add_overflow(start, 1)) {
      return GTEXT_JSON_E_INVALID; // Overflow
    }
    start++; // Move past opening quote
  }

  size_t string_start = start;
  size_t string_end = string_start;

  // Restore escape sequence state if resuming
  int in_escape = 0;
  int unicode_escape_remaining = 0;
  int high_surrogate_seen = 0;

  if (resuming && tb) {
    in_escape = tb->parse_state.string_state.in_escape;
    unicode_escape_remaining =
        tb->parse_state.string_state.unicode_escape_remaining;
    high_surrogate_seen = tb->parse_state.string_state.high_surrogate_seen;
  }

  // Find closing quote, tracking escape sequences
  while (string_end < lexer->input_len) {
    // Defensive bounds check before buffer access
    if (!json_check_bounds_offset(string_end, lexer->input_len)) {
      break;
    }
    char c = lexer->input[string_end];

    if (unicode_escape_remaining > 0) {
      // Parsing Unicode escape - accumulate hex digits
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')) {
        unicode_escape_remaining--;
        string_end++;
        // Append to buffer if available
        if (tb) {
          GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
          if (status != GTEXT_JSON_OK) {
            return status;
          }
        }
        continue;
      }
      else {
        // Invalid hex digit in Unicode escape
        if (tb) {
          json_token_buffer_clear(tb);
        }
        return GTEXT_JSON_E_BAD_UNICODE;
      }
    }

    if (in_escape) {
      in_escape = 0;
      if (c == 'u') {
        // Start of Unicode escape - need 4 more hex digits
        unicode_escape_remaining = 4;
        string_end++;
        // Append to buffer if available
        if (tb) {
          GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
          if (status != GTEXT_JSON_OK) {
            return status;
          }
        }
        continue;
      }
      else {
        // Standard escape character - consume it
        string_end++;
        // Append to buffer if available
        if (tb) {
          GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
          if (status != GTEXT_JSON_OK) {
            return status;
          }
        }
        continue;
      }
    }

    if (c == '\\') {
      in_escape = 1;
      string_end++;
      // Append to buffer if available
      if (tb) {
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
      continue;
    }

    if (c == quote_char) {
      // Found closing quote
      // Append closing quote to buffer if available
      if (tb) {
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
      break;
    }

    // Regular character
    string_end++;
    // Append to buffer if available
    if (tb) {
      GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
  }

  // Check if string is incomplete
  if (string_end >= lexer->input_len) {
    // String is incomplete - preserve state and return incomplete status
    if (tb) {
      tb->parse_state.string_state.in_escape = in_escape;
      tb->parse_state.string_state.unicode_escape_remaining =
          unicode_escape_remaining;
      tb->parse_state.string_state.high_surrogate_seen = high_surrogate_seen;
    }
    // Advance current_offset to string_end so the incomplete token gets removed
    // from input buffer
    lexer->current_offset = string_end;
    return GTEXT_JSON_E_INCOMPLETE;
  }

  // String is complete
  // string_start to string_end is the string content (without quotes)
  // Check for underflow in subtraction
  if (json_check_sub_underflow(string_end, string_start)) {
    if (tb) {
      json_token_buffer_clear(tb);
    }
    return GTEXT_JSON_E_INVALID; // Underflow
  }
  size_t string_content_len = string_end - string_start;

  // Get the complete string content from buffer or input
  const char * string_content;
  size_t string_content_actual_len;

  // If we used the buffer (either resuming or started buffering in this chunk),
  // use it
  if (tb && tb->is_buffered && tb->buffer_used >= 2) {
    // Use buffered content - buffer has: [opening quote][content][closing
    // quote] We need just the content without quotes for decoding
    string_content = tb->buffer + 1;                 // Skip opening quote
    string_content_actual_len = tb->buffer_used - 2; // Exclude both quotes
  }
  else {
    // Use input directly (no buffering was used - string was complete in one
    // chunk)
    string_content = lexer->input + string_start;
    string_content_actual_len = string_content_len;
  }

  // Check for integer overflow in token_length calculation
  size_t token_length;
  if (resuming && tb && tb->is_buffered) {
    token_length = tb->buffer_used; // Includes both quotes
  }
  else {
    size_t start_pos = start;
    if (resuming) {
      if (!tb) {
        return GTEXT_JSON_E_BAD_TOKEN;
      }
      start_pos = tb->start_offset;
    }
    // Check for overflow: string_end + 1
    if (json_check_add_overflow(string_end, 1)) {
      if (tb) {
        json_token_buffer_clear(tb);
      }
      return GTEXT_JSON_E_INVALID; // Overflow
    }
    size_t end_plus_one = string_end + 1;
    // Check for underflow: end_plus_one - start_pos
    if (json_check_sub_underflow(end_plus_one, start_pos)) {
      if (tb) {
        json_token_buffer_clear(tb);
      }
      return GTEXT_JSON_E_INVALID; // Underflow
    }
    token_length = end_plus_one - start_pos; // Include quotes
  }

  // Decode the string
  // Allocate buffer for decoded string (worst case: same size as input)
  // Check for integer overflow in allocation size using shared helper
  if (json_check_string_length_overflow(string_content_actual_len)) {
    if (tb) {
      json_token_buffer_clear(tb);
    }
    return GTEXT_JSON_E_LIMIT; // String too large
  }
  size_t decode_capacity =
      string_content_actual_len + 1; // +1 for null terminator
  char * decoded = (char *)gtext_allocator_malloc(
      lexer->opts ? lexer->opts->allocator : NULL, decode_capacity);
  if (!decoded) {
    if (tb) {
      json_token_buffer_clear(tb);
    }
    return GTEXT_JSON_E_OOM;
  }

  json_position decode_pos = lexer->pos;
  if (resuming && tb) {
    decode_pos.offset = tb->start_offset + 1; // After opening quote
  }
  else {
    decode_pos.offset = string_start;
  }
  decode_pos.col++; // After opening quote

  size_t decoded_len;
  GTEXT_JSON_Status status =
      json_decode_string(string_content, string_content_actual_len, decoded,
          decode_capacity, &decoded_len, &decode_pos, JSON_UTF8_REJECT,
          lexer->opts);

  if (status != GTEXT_JSON_OK) {
    gtext_allocator_free(
        lexer->opts ? lexer->opts->allocator : NULL, decoded);
    if (tb) {
      json_token_buffer_clear(tb);
    }
    return status;
  }

  /*
   * NFC, if it was asked for. This is the only place a JSON string becomes
   * bytes, so normalising here covers object names as well as values - which
   * is the point, because it means duplicate-name detection in the parser
   * compares names that have already been normalised. Two names that differ
   * only in how a mark was encoded are the same name, and a parser that
   * decided that before normalising would answer otherwise.
   *
   * The result always replaces the decoded buffer, even when normalisation
   * changed nothing: the alternative is two ownership paths out of one branch.
   */
  if (lexer->opts && lexer->opts->normalize_unicode) {
    char * normalized = NULL;
    size_t normalized_len = 0;
    int nfc = gtext_nfc_utf8(lexer->opts->allocator, decoded, decoded_len,
        &normalized, &normalized_len);
    gtext_allocator_free(lexer->opts->allocator, decoded);
    if (nfc != 1) {
      if (tb) {
        json_token_buffer_clear(tb);
      }
      return nfc == 0 ? GTEXT_JSON_E_BAD_UNICODE : GTEXT_JSON_E_OOM;
    }
    decoded = normalized;
    decoded_len = normalized_len;
  }

  token->type = JSON_TOKEN_STRING;
  token->pos = lexer->pos;
  token->length = token_length;
  token->data.string.alloc = lexer->opts ? lexer->opts->allocator : NULL;
  token->data.string.value = decoded;
  token->data.string.value_len = decoded_len;
  if (resuming && tb) {
    token->data.string.original_start = tb->start_offset + 1;
    token->data.string.original_len = string_content_actual_len;
  }
  else {
    token->data.string.original_start = string_start;
    token->data.string.original_len = string_content_len;
  }

  // Clear token buffer since string is complete
  if (tb) {
    json_token_buffer_clear(tb);
  }

  // Update lexer position
  // Check for overflow before adding string_end + 1
  if (json_check_add_overflow(string_end, 1)) {
    // Overflow - saturate at SIZE_MAX
    lexer->current_offset = SIZE_MAX;
    lexer->pos.offset = SIZE_MAX;
  }
  else {
    lexer->current_offset = string_end + 1;
    lexer->pos.offset = lexer->current_offset;
  }
  json_position_update_column(&lexer->pos, token_length);

  return GTEXT_JSON_OK;
}

// Parse a number token
static GTEXT_JSON_Status json_lexer_parse_number(
    json_lexer * lexer, json_token * token) {
  json_token_buffer * tb = lexer->token_buffer;
  size_t start = lexer->current_offset;
  size_t end = start;
  int resuming = 0;

  // Check if resuming from incomplete number
  int has_dot = 0;
  int has_exp = 0;
  int exp_sign_seen = 0;
  int starts_with_minus = 0;
  int starts_with_plus = 0;
  int is_hex = 0;
  const int allow_plus = lexer->opts && lexer->opts->allow_leading_plus;
  const int allow_hex = lexer->opts && lexer->opts->allow_hex_numbers;

  if (tb && tb->type == JSON_TOKEN_BUFFER_NUMBER) {
    resuming = 1;
    // Restore number state
    has_dot = tb->parse_state.number_state.has_dot;
    has_exp = tb->parse_state.number_state.has_exp;
    exp_sign_seen = tb->parse_state.number_state.exp_sign_seen;
    starts_with_minus = tb->parse_state.number_state.starts_with_minus;
    starts_with_plus = tb->parse_state.number_state.starts_with_plus;
    is_hex = tb->parse_state.number_state.is_hex;

    /* A signed nonfinite word that was cut in half by a chunk boundary. The
     * buffer holds the sign and however much of the word arrived; take as many
     * more bytes as keep it a prefix of one of the spellings. The main scan
     * loop below cannot do this - the letters are not number characters - and
     * whatever it leaves behind, the check before validation decides between
     * "complete" and "needs more input". */
    if ((starts_with_minus || starts_with_plus) && !has_dot && !has_exp &&
        !is_hex && tb->buffer_used > 0 && lexer->opts &&
        lexer->opts->allow_nonfinite_numbers &&
        json_signed_nonfinite_match(tb->buffer, tb->buffer_used, allow_plus) ==
            1) {
      while (end < lexer->input_len &&
          json_check_bounds_offset(end, lexer->input_len)) {
        char next_c = lexer->input[end];
        char candidate[16];
        if (tb->buffer_used + 1 > sizeof(candidate)) {
          break;
        }
        memcpy(candidate, tb->buffer, tb->buffer_used);
        candidate[tb->buffer_used] = next_c;
        if (!json_signed_nonfinite_match(
                candidate, tb->buffer_used + 1, allow_plus)) {
          break; // The next byte is not part of any spelling.
        }
        end++;
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &next_c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
    }
  }
  else {
    // Starting new number - initialize token buffer if available
    if (tb) {
      json_token_buffer_clear(tb);
      tb->type = JSON_TOKEN_BUFFER_NUMBER;
      tb->start_offset = start;
    }
  }

  // Determine number end by finding first non-number character
  // Numbers can contain: digits, '.', 'e', 'E', '+', '-'
  // When resuming with has_exp=1 but exp_sign_seen=0, we might need to handle
  // exponent sign first
  while (end < lexer->input_len) {
    // Defensive bounds check before buffer access
    if (!json_check_bounds_offset(end, lexer->input_len)) {
      break;
    }
    char c = lexer->input[end];

    // If resuming with exponent but no sign seen yet, check for exponent sign
    // first
    if (resuming && has_exp && !exp_sign_seen && (c == '+' || c == '-')) {
      exp_sign_seen = 1;
      end++;
      // Append to buffer if available
      if (tb) {
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
      continue;
    }

    /* The 'x' that opens a hex literal, and then its digits. 'e' is one of
     * those digits rather than an exponent marker, and a '.' ends the token
     * rather than opening a fraction, so both of those branches below are
     * closed while is_hex holds. */
    if (allow_hex && !is_hex && !has_dot && !has_exp && (c == 'x' || c == 'X')) {
      const size_t digits_so_far =
          resuming && tb ? tb->buffer_used : (end - start);
      const size_t sign_len = (starts_with_minus || starts_with_plus) ? 1u : 0u;
      const char zero = resuming && tb && tb->buffer_used > sign_len
          ? tb->buffer[sign_len]
          : (json_check_bounds_offset(start + sign_len, lexer->input_len)
                    ? lexer->input[start + sign_len]
                    : '\0');
      if (digits_so_far == sign_len + 1 && zero == '0') {
        is_hex = 1;
        end++;
        if (tb) {
          GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
          if (status != GTEXT_JSON_OK) {
            return status;
          }
        }
        continue;
      }
      // An 'x' anywhere else is not part of a number.
      break;
    }
    /* Hex letters, and this branch sits above the exponent one so that 'e'
     * and 'E' arrive here rather than there. The !is_hex on the exponent
     * branch is therefore belt-and-braces: it keeps the rule true if these
     * branches are ever reordered. */
    if (is_hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      end++;
      if (tb) {
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
      continue;
    }
    if (c >= '0' && c <= '9') {
      end++;
      // Append to buffer if available
      if (tb) {
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
      continue;
    }
    if (c == '.' && !has_dot && !has_exp && !is_hex) {
      has_dot = 1;
      end++;
      // Append to buffer if available
      if (tb) {
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
      continue;
    }
    if ((c == 'e' || c == 'E') && !has_exp && !is_hex) {
      has_exp = 1;
      exp_sign_seen = 0;
      end++;
      // Append to buffer if available
      if (tb) {
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
      // Exponent can have + or -
      // Defensive bounds check before buffer access
      if (end < lexer->input_len &&
          json_check_bounds_offset(end, lexer->input_len) &&
          (lexer->input[end] == '+' || lexer->input[end] == '-')) {
        exp_sign_seen = 1;
        end++;
        // Append to buffer if available
        if (tb) {
          // Defensive bounds check for end - 1
          if (end > 0 && json_check_bounds_offset(end - 1, lexer->input_len)) {
            GTEXT_JSON_Status status =
                json_token_buffer_append(tb, &lexer->input[end - 1], 1);
            if (status != GTEXT_JSON_OK) {
              return status;
            }
          }
        }
      }
      continue;
    }
    if ((c == '-' || (allow_plus && c == '+')) && end == start && !resuming) {
      // Leading sign
      if (c == '-') {
        starts_with_minus = 1;
      }
      else {
        starts_with_plus = 1;
      }
      end++;
      // Append to buffer if available
      if (tb) {
        GTEXT_JSON_Status status = json_token_buffer_append(tb, &c, 1);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
      continue;
    }

    // Check if this could be part of -Infinity (if enabled and we started with
    // minus) When we encounter a non-number character after '-', check if it's
    // 'I' (start of "Infinity")
    /* The sign was consumed above and a letter follows, so this may be a
     * signed nonfinite word rather than a number: "-Infinity" as JSON's
     * extensions spell it, or one of the three JSON5 adds. Take bytes while
     * they keep the token a prefix of one of the spellings, then leave the
     * loop - the check before validation decides between a complete word, an
     * unfinished one, and something that is neither. */
    if ((starts_with_minus || starts_with_plus) && !has_dot && !has_exp &&
        !is_hex && lexer->opts && lexer->opts->allow_nonfinite_numbers &&
        (end == start + 1) && (c == 'I' || c == 'N')) {
      char candidate[16];
      size_t candidate_len = 1;
      candidate[0] = lexer->input[start];
      while (end < lexer->input_len &&
          json_check_bounds_offset(end, lexer->input_len) &&
          candidate_len + 1 <= sizeof(candidate)) {
        char next_c = lexer->input[end];
        candidate[candidate_len] = next_c;
        if (!json_signed_nonfinite_match(
                candidate, candidate_len + 1, allow_plus)) {
          break; // The next byte is not part of any spelling.
        }
        candidate_len++;
        end++;
        if (tb) {
          GTEXT_JSON_Status status = json_token_buffer_append(tb, &next_c, 1);
          if (status != GTEXT_JSON_OK) {
            return status;
          }
        }
      }
      break;
    }

    // Not part of number
    break;
  }

  // Check if number might be incomplete (at EOF) - do this BEFORE validation
  // because an incomplete number (like just "-" or "123") is not invalid, just
  // incomplete
  if (end >= lexer->input_len && end > start) {
    // We're at EOF - check if the number might continue
    // Defensive check: ensure end - 1 is valid (end > start guarantees end >=
    // 1)
    // Special case: if we only have "-" (just a minus sign), it's incomplete
    size_t total_len = resuming && tb ? tb->buffer_used : (end - start);
    if (total_len == 1) {
      // Defensive bounds check before buffer access
      char first_char = resuming && tb
          ? tb->buffer[0]
          : (json_check_bounds_offset(start, lexer->input_len)
                    ? lexer->input[start]
                    : '\0');
      if (first_char == '-' || (allow_plus && first_char == '+')) {
        // Just a sign - incomplete, need digits
        if (tb) {
          tb->parse_state.number_state.has_dot = has_dot;
          tb->parse_state.number_state.has_exp = has_exp;
          tb->parse_state.number_state.exp_sign_seen = exp_sign_seen;
          tb->parse_state.number_state.starts_with_minus = starts_with_minus;
          tb->parse_state.number_state.starts_with_plus = starts_with_plus;
          tb->parse_state.number_state.is_hex = is_hex;
        }
        lexer->current_offset = end;
        return GTEXT_JSON_E_INCOMPLETE;
      }
    }

    // Determine the last character of the number
    // When resuming, check the buffer's last character (it has the complete
    // token so far) When not resuming, check the input's last character
    char last_char;
    if (resuming && tb && tb->buffer_used > 0) {
      // Resuming - buffer has the complete token so far
      last_char = tb->buffer[tb->buffer_used - 1];
    }
    else {
      // Not resuming - check input's last character
      // At this point, end == input_len (loop ensures end <= input_len, and we
      // check end >= input_len) So end - 1 == input_len - 1, which is the last
      // valid index Defensive bounds check before buffer access
      if (end > 0 && json_check_bounds_offset(end - 1, lexer->input_len)) {
        last_char = lexer->input[end - 1];
      }
      else {
        last_char = '\0'; // Fallback if bounds check fails
      }
    }

    // If ends with '.', 'e', 'E', '+', or '-' (exponent sign), clearly
    // incomplete For '-', check if it's an exponent sign (has_exp) - if so,
    // it's incomplete
    int trailing_incomplete;
    if (is_hex) {
      /* A hex literal has no exponent and no fraction: 'e' is one of its
       * digits and 'x' is its opening. So nothing about the last character
       * says the token is unfinished, and only a stream that may still send
       * more digits makes it so. In a complete buffer "0x" is left to the
       * validator, which refuses it as a number with no digits. */
      trailing_incomplete = lexer->streaming_mode;
    }
    else if (last_char == '.') {
      /* A point at the edge is a number in its own right where JSON5 asks
       * for it, so only a stream that may still send digits makes it
       * unfinished. */
      trailing_incomplete =
          !(lexer->opts && lexer->opts->allow_bare_decimal_point) ||
          lexer->streaming_mode;
    }
    else {
      trailing_incomplete = last_char == 'e' || last_char == 'E' ||
          last_char == '+' || (last_char == '-' && has_exp);
    }
    if (trailing_incomplete) {
      // Number ends with incomplete indicator - preserve state and return
      // incomplete
      if (tb) {
        tb->parse_state.number_state.has_dot = has_dot;
        tb->parse_state.number_state.has_exp = has_exp;
        tb->parse_state.number_state.exp_sign_seen = exp_sign_seen;
        tb->parse_state.number_state.starts_with_minus = starts_with_minus;
        tb->parse_state.number_state.starts_with_plus = starts_with_plus;
        tb->parse_state.number_state.is_hex = is_hex;
      }
      // Advance current_offset to end so the incomplete token gets removed from
      // input buffer
      lexer->current_offset = end;
      return GTEXT_JSON_E_INCOMPLETE;
    }

    // In streaming mode, if number ends with a digit at EOF, it might continue
    // with '.', 'e', or 'E' in the next chunk, so treat as incomplete
    // EXCEPTIONS when resuming:
    // 1. If number has an exponent with sign and digits, it's complete
    // (exponent is complete) NOTE: A number with a dot but no exponent can
    // still continue with 'e' or 'E', so it's incomplete
    if (lexer->streaming_mode && last_char >= '0' && last_char <= '9') {
      // Check if number is actually complete when resuming
      if (resuming && tb) {
        // A number with a dot but no exponent can still continue with 'e' or
        // 'E' So we don't treat it as complete here - it might continue (We
        // used to treat it as complete, but that was wrong) In streaming mode,
        // numbers ending with a digit at EOF are always incomplete because they
        // could continue with more digits (e.g., "1.5e+2" could become
        // "1.5e+20") The only exception is when we're in non-streaming mode
        // (finish() was called) Check if number ends with exponent sign - if
        // so, it's incomplete
        if (has_exp && exp_sign_seen) {
          char last_char_in_buffer = tb->buffer[tb->buffer_used - 1];
          if (last_char_in_buffer == '+' || last_char_in_buffer == '-') {
            // Ends with exponent sign - incomplete, need digits
            tb->parse_state.number_state.has_dot = has_dot;
            tb->parse_state.number_state.has_exp = has_exp;
            tb->parse_state.number_state.exp_sign_seen = exp_sign_seen;
            tb->parse_state.number_state.starts_with_minus = starts_with_minus;
            tb->parse_state.number_state.starts_with_plus = starts_with_plus;
            tb->parse_state.number_state.is_hex = is_hex;
            lexer->current_offset = end;
            return GTEXT_JSON_E_INCOMPLETE;
          }
          // Has exponent with sign and digits, but ends with digit at EOF
          // In streaming mode, this is incomplete (could continue with more
          // digits) Fall through to preserve state and return incomplete
        }
        {
          // Preserve state - number might continue
          tb->parse_state.number_state.has_dot = has_dot;
          tb->parse_state.number_state.has_exp = has_exp;
          tb->parse_state.number_state.exp_sign_seen = exp_sign_seen;
          tb->parse_state.number_state.starts_with_minus = starts_with_minus;
          tb->parse_state.number_state.starts_with_plus = starts_with_plus;
          tb->parse_state.number_state.is_hex = is_hex;
          // Advance current_offset to end so the incomplete token gets removed
          // from input buffer
          lexer->current_offset = end;
          return GTEXT_JSON_E_INCOMPLETE;
        }
      }
      else {
        // Not resuming - preserve state, number might continue
        if (tb) {
          tb->parse_state.number_state.has_dot = has_dot;
          tb->parse_state.number_state.has_exp = has_exp;
          tb->parse_state.number_state.exp_sign_seen = exp_sign_seen;
          tb->parse_state.number_state.starts_with_minus = starts_with_minus;
          tb->parse_state.number_state.starts_with_plus = starts_with_plus;
          tb->parse_state.number_state.is_hex = is_hex;
        }
        // Advance current_offset to end so the incomplete token gets removed
        // from input buffer
        lexer->current_offset = end;
        return GTEXT_JSON_E_INCOMPLETE;
      }
    }

    // At EOF but number looks complete - parse it
  }

  /* How much of the token has been seen. When resuming, the buffer holds all
   * of it - every branch of the scan loop appends what it consumes - so adding
   * (end - start) counted this chunk's bytes twice. The EOF block above had it
   * right and this had it wrong, and the disagreement is what kept
   * "-Infinity" from streaming at an even chunk size: with the buffer at
   * "-Infinit" the doubled figure passed 9, the signed-nonfinite prefix check
   * below was skipped as too long to be one, and an unfinished word went to
   * json_parse_number() as though it were finished. */
  size_t total_len = resuming && tb ? tb->buffer_used : (end - start);

  // Before validating, check if this might be -Infinity (complete or prefix)
  // This handles the case where -Infinity wasn't caught by
  // json_lexer_match_neg_infinity because it was called before we had enough
  // characters, or in streaming mode We need to check if what we have matches
  // "-Infinity" or is a prefix of it
  if ((starts_with_minus || starts_with_plus) && !is_hex && total_len >= 1 &&
      total_len <= 9 && lexer->opts && lexer->opts->allow_nonfinite_numbers) {
    const char * number_content;
    size_t content_len;
    if (resuming && tb && tb->buffer_used > 0) {
      number_content = tb->buffer;
      content_len = tb->buffer_used;
    }
    else {
      number_content = lexer->input + start;
      content_len = end - start;
    }
    // Exactly one of the signed nonfinite spellings, a prefix of one, or
    // neither.
    const int nonfinite_match =
        json_signed_nonfinite_match(number_content, content_len, allow_plus);
    if (nonfinite_match) {
      if (nonfinite_match == 2) {
        // A complete word - json_parse_number() below recognises it, from the
        // buffered content when resuming.
      }
      else {
        // Incomplete -Infinity prefix - treat as incomplete
        // Need to ensure characters are buffered
        if (tb) {
          // Mark that we're buffering a number
          if (tb->type == JSON_TOKEN_BUFFER_NONE) {
            tb->type = JSON_TOKEN_BUFFER_NUMBER;
            tb->start_offset = start;
          }
          /* The characters are already in the buffer: every branch of the scan
           * loop appends what it consumes, and json_token_buffer_append() sets
           * is_buffered itself. This used to append them a second time, so a
           * first feed holding a sign and at least one letter of a nonfinite
           * word left "-I-I" in the buffer and the next feed lexed that - which
           * is why "-Infinity" could not be streamed at any chunk size between
           * 2 and 8. At chunk size 1 the first feed is the sign alone, which
           * returns through a different path and was never doubled. */
          tb->parse_state.number_state.has_dot = has_dot;
          tb->parse_state.number_state.has_exp = has_exp;
          tb->parse_state.number_state.exp_sign_seen = exp_sign_seen;
          tb->parse_state.number_state.starts_with_minus = starts_with_minus;
          tb->parse_state.number_state.starts_with_plus = starts_with_plus;
          tb->parse_state.number_state.is_hex = is_hex;
        }
        // Advance current_offset to end so the incomplete token gets removed
        // from input buffer The token buffer has the data, so it's safe to mark
        // input as processed
        lexer->current_offset = end;
        return GTEXT_JSON_E_INCOMPLETE;
      }
    }
  }

  // Defensive bounds check before buffer access
  char first_char_check = resuming && tb
      ? tb->buffer[0]
      : (json_check_bounds_offset(start, lexer->input_len) ? lexer->input[start]
                                                           : '\0');
  if (total_len == 0 ||
      (total_len == 1 && (first_char_check == '-' || first_char_check == '+'))) {
    if (tb) {
      json_token_buffer_clear(tb);
    }
    return GTEXT_JSON_E_BAD_NUMBER;
  }

  // Number is complete - get the complete number content
  // If we appended to buffer in the loop, use buffer; otherwise use input
  // directly
  const char * number_content;
  size_t number_len;

  // Use buffer if: resuming (buffer has previous chunk data) OR buffer was used
  // in this chunk
  if (tb && tb->buffer_used > 0 && (resuming || tb->is_buffered)) {
    // Use buffered content (was appended in loop above)
    number_content = tb->buffer;
    number_len = tb->buffer_used;
  }
  else {
    // Use input directly (no buffering was needed)
    number_content = lexer->input + start;
    number_len = end - start;
  }

  // Parse the number
  json_position num_pos = lexer->pos;
  GTEXT_JSON_Status status = json_parse_number(
      number_content, number_len, &token->data.number, &num_pos, lexer->opts);

  if (status != GTEXT_JSON_OK) {
    if (tb) {
      json_token_buffer_clear(tb);
    }
    return status;
  }

  token->type = JSON_TOKEN_NUMBER;
  token->pos = lexer->pos;
  if (resuming && tb) {
    token->length = tb->buffer_used;
  }
  else {
    token->length = number_len;
  }

  // Clear token buffer since number is complete
  if (tb) {
    json_token_buffer_clear(tb);
  }

  // Update lexer position
  // When resuming from buffer, if end == start (no new input consumed),
  // we should NOT advance past the non-number character - it needs to be
  // processed as the next token (e.g., closing bracket, comma, etc.)
  // The next call to json_lexer_next() will process it
  if (resuming && end == start) {
    // We completed the number from the buffer, but didn't consume any new input
    // The character at position 0 is the non-number character that ended the
    // number We should NOT advance past it - leave it for the next token
  }
  lexer->current_offset = end;
  lexer->pos.offset = lexer->current_offset;
  json_position_update_column(&lexer->pos, token->length);

  return GTEXT_JSON_OK;
}

GTEXT_INTERNAL_API GTEXT_JSON_Status json_lexer_init(json_lexer * lexer,
    const char * input, size_t input_len, const GTEXT_JSON_Parse_Options * opts,
    int streaming_mode, int at_input_start) {
  // Defensive NULL pointer checks
  if (!lexer) {
    return GTEXT_JSON_E_INVALID;
  }
  if (!input && input_len > 0) {
    return GTEXT_JSON_E_INVALID;
  }

  lexer->input = input;
  lexer->input_len = input_len;
  lexer->current_offset = 0;
  lexer->pos.offset = 0;
  lexer->pos.line = 1;
  lexer->pos.col = 1;
  lexer->opts = opts;
  lexer->streaming_mode = streaming_mode ? 1 : 0;
  lexer->token_buffer = NULL; // Set by caller if needed

  /* Skip leading BOM if enabled - and only where the input really begins.
   * The streaming parser re-initialises the lexer on every feed with whatever
   * is unprocessed, and the buffer is compacted, so without at_input_start a
   * mid-document BOM was skipped as a leading one whenever a chunk boundary
   * left it at the front of the buffer: `[1,<BOM>2]` was refused by
   * gtext_json_parse() and accepted by the stream at a chunk size of 3. */
  if (at_input_start && opts && opts->allow_leading_bom && input_len >= 3 &&
      (unsigned char)input[0] == 0xEF && (unsigned char)input[1] == 0xBB &&
      (unsigned char)input[2] == 0xBF) {
    lexer->current_offset = 3;
    lexer->pos.offset = 3;
    lexer->pos.col = 4;
  }

  return GTEXT_JSON_OK;
}

GTEXT_INTERNAL_API GTEXT_JSON_Status json_lexer_next(
    json_lexer * lexer, json_token * token) {
  // Defensive NULL pointer checks
  if (!lexer) {
    return GTEXT_JSON_E_INVALID;
  }
  if (!token) {
    return GTEXT_JSON_E_INVALID;
  }
  // Additional defensive check: ensure input is valid if we have input_len > 0
  if (lexer->input_len > 0 && !lexer->input) {
    return GTEXT_JSON_E_INVALID;
  }

  // Initialize token
  memset(token, 0, sizeof(*token));
  token->type = JSON_TOKEN_ERROR;

  // Check if we're resuming from an incomplete token
  if (lexer->token_buffer &&
      lexer->token_buffer->type != JSON_TOKEN_BUFFER_NONE) {
    // We're resuming - directly call the appropriate parse function
    if (lexer->token_buffer->type == JSON_TOKEN_BUFFER_STRING) {
      return json_lexer_parse_string(lexer, token);
    }
    else if (lexer->token_buffer->type == JSON_TOKEN_BUFFER_NUMBER) {
      return json_lexer_parse_number(lexer, token);
    }
  }

  // Skip whitespace
  json_lexer_skip_whitespace(lexer);

  // Skip comments (if enabled)
  GTEXT_JSON_Status status = json_lexer_skip_comments(lexer);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  // Check for EOF
  if (lexer->current_offset >= lexer->input_len) {
    token->type = JSON_TOKEN_EOF;
    token->pos = lexer->pos;
    token->length = 0;
    return GTEXT_JSON_OK;
  }

  size_t start = lexer->current_offset;
  // Defensive bounds check before buffer access
  if (!json_check_bounds_offset(start, lexer->input_len)) {
    token->type = JSON_TOKEN_ERROR;
    token->pos = lexer->pos;
    token->length = 0;
    return GTEXT_JSON_E_INVALID;
  }
  char c = lexer->input[start];

  /* A multi-byte character cut in half by a chunk boundary. Between tokens
   * nothing may start with a byte above 0x7F except a byte-order mark and, with
   * allow_ecma_whitespace, one of Unicode's spaces - and the skippers above
   * cannot tell "this is not one of those" from "not all of it has arrived", so
   * more input is the only thing that can decide. A byte that cannot start a
   * character at all is a real error and falls through to one.
   *
   * This used to ask for allow_ecma_whitespace, which left strict JSON unable
   * to take a BOM that arrived in pieces: with the bytes fed one at a time,
   * `<BOM>1` was accepted by gtext_json_parse() and refused by the stream. */
  if (lexer->streaming_mode && (unsigned char)c >= 0x80) {
    const size_t need = json_utf8_lead_length((unsigned char)c);
    if (need > 1 && lexer->input_len - start < need) {
      return GTEXT_JSON_E_INCOMPLETE;
    }
  }

  // Punctuation tokens
  switch (c) {
  case '{':
    token->type = JSON_TOKEN_LBRACE;
    token->pos = lexer->pos;
    token->length = 1;
    // Check for overflow before incrementing
    if (json_check_add_overflow(lexer->current_offset, 1)) {
      lexer->current_offset = SIZE_MAX;
    }
    else {
      lexer->current_offset++;
    }
    json_position_update_offset(&lexer->pos, 1);
    json_position_update_column(&lexer->pos, 1);
    return GTEXT_JSON_OK;

  case '}':
    token->type = JSON_TOKEN_RBRACE;
    token->pos = lexer->pos;
    token->length = 1;
    // Check for overflow before incrementing
    if (json_check_add_overflow(lexer->current_offset, 1)) {
      lexer->current_offset = SIZE_MAX;
    }
    else {
      lexer->current_offset++;
    }
    json_position_update_offset(&lexer->pos, 1);
    json_position_update_column(&lexer->pos, 1);
    return GTEXT_JSON_OK;

  case '[':
    token->type = JSON_TOKEN_LBRACKET;
    token->pos = lexer->pos;
    token->length = 1;
    // Check for overflow before incrementing
    if (json_check_add_overflow(lexer->current_offset, 1)) {
      lexer->current_offset = SIZE_MAX;
    }
    else {
      lexer->current_offset++;
    }
    json_position_update_offset(&lexer->pos, 1);
    json_position_update_column(&lexer->pos, 1);
    return GTEXT_JSON_OK;

  case ']':
    token->type = JSON_TOKEN_RBRACKET;
    token->pos = lexer->pos;
    token->length = 1;
    // Check for overflow before incrementing
    if (json_check_add_overflow(lexer->current_offset, 1)) {
      lexer->current_offset = SIZE_MAX;
    }
    else {
      lexer->current_offset++;
    }
    json_position_update_offset(&lexer->pos, 1);
    json_position_update_column(&lexer->pos, 1);
    return GTEXT_JSON_OK;

  case ':':
    token->type = JSON_TOKEN_COLON;
    token->pos = lexer->pos;
    token->length = 1;
    // Check for overflow before incrementing
    if (json_check_add_overflow(lexer->current_offset, 1)) {
      lexer->current_offset = SIZE_MAX;
    }
    else {
      lexer->current_offset++;
    }
    json_position_update_offset(&lexer->pos, 1);
    json_position_update_column(&lexer->pos, 1);
    return GTEXT_JSON_OK;

  case ',':
    token->type = JSON_TOKEN_COMMA;
    token->pos = lexer->pos;
    token->length = 1;
    // Check for overflow before incrementing
    if (json_check_add_overflow(lexer->current_offset, 1)) {
      lexer->current_offset = SIZE_MAX;
    }
    else {
      lexer->current_offset++;
    }
    json_position_update_offset(&lexer->pos, 1);
    json_position_update_column(&lexer->pos, 1);
    return GTEXT_JSON_OK;
  }

  // String tokens
  if (c == '"' ||
      (lexer->opts && lexer->opts->allow_single_quotes && c == '\'')) {
    return json_lexer_parse_string(lexer, token);
  }

  // Number tokens (including the signed nonfinite special cases)
  // Note: -Infinity and friends are checked here because they start with a
  // sign, which is also the start of a signed number. They are gated behind
  // allow_nonfinite_numbers (same as "Infinity" in the keyword path), and the
  // '+' spellings additionally behind allow_leading_plus.
  const int lex_allow_plus = lexer->opts && lexer->opts->allow_leading_plus;
  const int lex_allow_bare_point =
      lexer->opts && lexer->opts->allow_bare_decimal_point;
  if (c == '-' || (c >= '0' && c <= '9') || (lex_allow_plus && c == '+') ||
      (lex_allow_bare_point && c == '.')) {
    // Check for a signed nonfinite first (if enabled)
    if (c == '-' || c == '+') {
      int nonfinite_result = json_lexer_match_signed_nonfinite(lexer, token);
      if (nonfinite_result == 1) {
        return GTEXT_JSON_OK;
      }
      else if (nonfinite_result == GTEXT_JSON_E_NONFINITE) {
        return GTEXT_JSON_E_NONFINITE;
      }
      // Otherwise (0), continue to number parsing
    }
    return json_lexer_parse_number(lexer, token);
  }

  const int allow_unquoted =
      lexer->opts && lexer->opts->allow_unquoted_keys;

  // Keyword tokens (true, false, null, NaN, Infinity)
  const json_position keyword_pos = lexer->pos;
  int keyword_result = json_lexer_match_keyword(lexer, token);
  if (keyword_result == 1) {
    return GTEXT_JSON_OK;
  }
  else if (keyword_result == GTEXT_JSON_E_INCOMPLETE) {
    /* The run reached the end of the buffer, so the word is not settled yet.
     * Nothing was consumed; the stream keeps the bytes. */
    return GTEXT_JSON_E_INCOMPLETE;
  }
  else if (keyword_result == GTEXT_JSON_E_NONFINITE) {
    /* "NaN" or "Infinity" where nonfinite numbers were not asked for. Where
     * unquoted names are allowed, those words are also ordinary names - JSON5
     * takes IdentifierName, which includes the reserved words - so the
     * identifier scan gets a look before this becomes an error. The refusal
     * advanced the lexer, so put it back first. */
    if (!allow_unquoted) {
      return GTEXT_JSON_E_NONFINITE;
    }
    lexer->current_offset = start;
    lexer->pos = keyword_pos;
    memset(token, 0, sizeof(*token));
    token->type = JSON_TOKEN_ERROR;
  }

  /* An unquoted object name. This sits after the keyword path so that `true`
   * in a value position is still the boolean; a name that happens to spell a
   * keyword is handled by the parsers, which know they are looking at a name.
   * A backslash can start one, because \uXXXX is a spelling of a character. */
  if (allow_unquoted) {
    GTEXT_JSON_Status ident_status = GTEXT_JSON_OK;
    if (json_lexer_parse_identifier(lexer, token, &ident_status)) {
      return ident_status;
    }
  }

  // In streaming mode, check if we have a partial keyword prefix
  // This allows keywords to span chunk boundaries
  if (lexer->streaming_mode && json_is_identifier_start(c)) {
    // We have an identifier start character but keyword didn't match
    // Check if it's a valid keyword prefix - if so, it's incomplete, not bad
    size_t prefix_len = 0;
    while (prefix_len < lexer->input_len) {
      // Check for underflow: input_len - start
      if (json_check_sub_underflow(lexer->input_len, start) ||
          prefix_len >= lexer->input_len - start) {
        break;
      }
      // Check for overflow: start + prefix_len
      if (json_check_add_overflow(start, prefix_len)) {
        break;
      }
      // Defensive bounds check before buffer access
      if (!json_check_bounds_offset(start + prefix_len, lexer->input_len)) {
        break;
      }
      if (!json_is_identifier_cont(lexer->input[start + prefix_len])) {
        break;
      }
      // Check for overflow before incrementing prefix_len
      if (json_check_add_overflow(prefix_len, 1)) {
        break;
      }
      prefix_len++;
    }

    if (prefix_len > 0 &&
        json_is_keyword_prefix(lexer->input + start, prefix_len)) {
      // Valid keyword prefix but incomplete - need more input
      // Don't advance current_offset - keep the partial keyword in the buffer
      // The stream parser will handle this by not marking input as processed
      return GTEXT_JSON_E_INCOMPLETE;
    }
  }

  // Unknown token
  token->type = JSON_TOKEN_ERROR;
  token->pos = lexer->pos;
  token->length = 1;
  return GTEXT_JSON_E_BAD_TOKEN;
}

GTEXT_INTERNAL_API void json_token_cleanup(json_token * token) {
  if (!token) {
    return;
  }

  switch (token->type) {
  case JSON_TOKEN_IDENT:
  case JSON_TOKEN_STRING:
    if (token->data.string.value) {
      gtext_allocator_free(
          token->data.string.alloc, token->data.string.value);
      token->data.string.value = NULL;
      token->data.string.value_len = 0;
    }
    break;

  case JSON_TOKEN_NUMBER:
    json_number_destroy(&token->data.number);
    break;

  default:
    break;
  }
}
