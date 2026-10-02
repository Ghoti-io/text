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
 * Number parsing and multi-representation support for JSON module.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "json_internal.h"
#include "../text_number_internal.h"

#include <ghoti.io/text/json/json_core.h>
// Check if a character is a digit
static int json_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int json_is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
      (c >= 'A' && c <= 'F');
}

// Validate number syntax according to RFC 8259, plus the JSON5 numeric
// literal forms each option asks for.
// Validates that the number follows JSON number grammar:
// - No leading zeros (except 0 itself)
// - No trailing decimal point (1. is invalid unless allow_bare_decimal_point)
// - No leading decimal point (.1 is invalid unless allow_bare_decimal_point)
// - No leading plus (+1 is invalid unless allow_leading_plus)
// - No hexadecimal (0x10 is invalid unless allow_hex_numbers)
// - Proper exponent format
// - No invalid characters
static int json_validate_number_syntax(const char * input, size_t len,
    size_t start_pos, const GTEXT_JSON_Parse_Options * opts) {
  if (len == 0 || start_pos >= len) {
    return 0;
  }

  const int allow_plus = opts && opts->allow_leading_plus;
  const int allow_hex = opts && opts->allow_hex_numbers;
  const int allow_bare_point = opts && opts->allow_bare_decimal_point;

  size_t i = start_pos;
  int has_digit = 0;

  // Optional sign
  if (input[i] == '-' || (allow_plus && input[i] == '+')) {
    i++;
  }

  // Hexadecimal, which has no fraction and no exponent: `e` is a digit here.
  if (allow_hex && i + 1 < len && input[i] == '0' &&
      (input[i + 1] == 'x' || input[i + 1] == 'X')) {
    i += 2;
    if (i >= len || !json_is_hex_digit(input[i])) {
      return 0; // "0x" with no digits
    }
    while (i < len && json_is_hex_digit(input[i])) {
      i++;
    }
    return i == len;
  }

  // Integer part
  if (allow_bare_point && i < len && input[i] == '.') {
    // ".5": no integer part at all. The fractional part below insists on a
    // digit, so "." and ".e1" are still refused.
  }
  else if (i < len && input[i] == '0') {
    // Leading zero - must be followed by . or end or e/E
    i++;
    has_digit = 1;
    if (i < len && json_is_digit(input[i])) {
      // Leading zero followed by digit (e.g., 01) is invalid
      return 0;
    }
  }
  else if (i < len && json_is_digit(input[i])) {
    // One or more digits
    while (i < len && json_is_digit(input[i])) {
      i++;
      has_digit = 1;
    }
  }
  else {
    // Must start with digit or minus followed by digit
    return 0;
  }

  // Fractional part (optional)
  if (i < len && input[i] == '.') {
    i++;
    // Must have at least one digit after the decimal point, unless the point
    // is allowed at an edge - and then there must be a digit on the other
    // side, so that "." alone is still not a number.
    if (i >= len || !json_is_digit(input[i])) {
      if (!allow_bare_point || !has_digit) {
        return 0; // Invalid: 1. or .1 (without leading zero)
      }
    }
    while (i < len && json_is_digit(input[i])) {
      i++;
      has_digit = 1;
    }
  }

  // Exponent part (optional)
  if (i < len && (input[i] == 'e' || input[i] == 'E')) {
    i++;
    // Optional sign
    if (i < len && (input[i] == '+' || input[i] == '-')) {
      i++;
    }
    // Must have at least one digit after e/E
    if (i >= len || !json_is_digit(input[i])) {
      return 0; // Invalid: 1e or 1e+
    }
    while (i < len && json_is_digit(input[i])) {
      i++;
    }
  }

  // Must have consumed entire input
  if (i != len) {
    return 0;
  }

  // Must have at least one digit
  return has_digit;
}

// Parse nonfinite number (NaN, Infinity, -Infinity)
/* The six spellings of a nonfinite value. "-NaN" and the two '+' forms are
 * JSON5's, whose grammar puts an optional sign in front of the whole value;
 * the '+' ones are only recognised where a leading plus was asked for, so that
 * json_parse_number() refuses "+NaN" for the same reason the lexer does not
 * offer it. NaN's sign is not observable, so both signed spellings give NaN. */
static int json_parse_nonfinite(
    const char * input, size_t len, json_number * num, int allow_plus) {
  double value;
  if (json_matches(input, len, "NaN") || json_matches(input, len, "-NaN")) {
    value = NAN;
  }
  else if (json_matches(input, len, "Infinity")) {
    value = INFINITY;
  }
  else if (json_matches(input, len, "-Infinity")) {
    value = -INFINITY;
  }
  else if (allow_plus && json_matches(input, len, "+NaN")) {
    value = NAN;
  }
  else if (allow_plus && json_matches(input, len, "+Infinity")) {
    value = INFINITY;
  }
  else {
    return 0;
  }
  num->dbl = value;
  num->flags = JSON_NUMBER_HAS_DOUBLE | JSON_NUMBER_IS_NONFINITE;
  return 1;
}

// Parse uint64 from string with overflow detection
static int json_parse_uint64(
    const char * input, size_t len, uint64_t * out_u64) {
  if (len == 0) {
    return 0;
  }

  // uint64 cannot be negative
  if (input[0] == '-') {
    return 0;
  }

  size_t i = 0;

  // A leading '+' is JSON5's, and the validator has already decided whether
  // it was allowed to be there. Digits mean the same either way.
  if (input[i] == '+') {
    i++;
    if (i >= len) {
      return 0;
    }
  }

  // Skip leading zeros
  while (i < len && input[i] == '0') {
    i++;
  }

  // If all zeros, result is 0
  if (i >= len) {
    *out_u64 = 0;
    return 1;
  }

  // Check if we have a fractional part or exponent
  // For now, only parse pure integers
  for (size_t j = i; j < len; ++j) {
    if (input[j] == '.' || input[j] == 'e' || input[j] == 'E') {
      // Has fractional part or exponent - not a pure integer
      return 0;
    }
  }

  // Parse digits
  uint64_t result = 0;
  uint64_t max_before_mult = UINT64_MAX / 10;
  uint64_t max_last_digit = UINT64_MAX % 10;

  for (; i < len; ++i) {
    if (!json_is_digit(input[i])) {
      return 0;
    }

    int digit = input[i] - '0';

    // Check for overflow before multiplying
    if (result > max_before_mult ||
        (result == max_before_mult && (uint64_t)digit > max_last_digit)) {
      return 0; // Overflow
    }

    result = result * 10 + digit;
  }

  *out_u64 = result;
  return 1;
}

// Parse int64 from string with overflow detection
// This function delegates to json_parse_uint64() after handling the sign
static int json_parse_int64(const char * input, size_t len, int64_t * out_i64) {
  if (len == 0) {
    return 0;
  }

  // Check for negative sign
  int negative = 0;
  size_t start = 0;
  if (input[0] == '-') {
    negative = 1;
    start = 1;
    // After '-', we need at least one digit
    if (start >= len) {
      return 0;
    }
  }

  // Parse as uint64 (this handles leading zeros, fractional/exponent checks,
  // etc.)
  uint64_t u64_val;
  if (!json_parse_uint64(input + start, len - start, &u64_val)) {
    return 0;
  }

  // Check if the value fits in int64 range
  if (negative) {
    // For negative numbers: the absolute value must be <= INT64_MAX + 1
    // Since -INT64_MIN = INT64_MAX + 1, we check: u64_val <=
    // (uint64_t)INT64_MAX + 1
    if (u64_val > (uint64_t)INT64_MAX + 1) {
      return 0; // Underflow: absolute value too large
    }
    // Special case: if u64_val == INT64_MAX + 1, result is INT64_MIN
    if (u64_val == (uint64_t)INT64_MAX + 1) {
      *out_i64 = INT64_MIN;
    }
    else {
      *out_i64 = -(int64_t)u64_val;
    }
  }
  else {
    // For positive numbers: must be <= INT64_MAX
    if (u64_val > (uint64_t)INT64_MAX) {
      return 0; // Overflow: value too large for int64
    }
    *out_i64 = (int64_t)u64_val;
  }

  return 1;
}

/* Where a JSON5 hex literal's digits begin, or 0 if this is not one. The
 * caller has already had the spelling validated, so this only has to find the
 * "0x" past an optional sign. */
static size_t json_hex_digits_at(
    const char * input, size_t len, const GTEXT_JSON_Parse_Options * opts) {
  if (!opts || !opts->allow_hex_numbers) {
    return 0;
  }
  size_t i = 0;
  if (i < len && (input[i] == '-' || input[i] == '+')) {
    i++;
  }
  if (i + 1 < len && input[i] == '0' &&
      (input[i + 1] == 'x' || input[i + 1] == 'X')) {
    return i + 2;
  }
  return 0;
}

/* A hex literal's magnitude. Returns 0 on overflow, which leaves the number
 * with its lexeme and its double and no integer, exactly as a decimal integer
 * too large for uint64 is left. */
static int json_parse_hex_u64(
    const char * input, size_t len, size_t from, uint64_t * out_u64) {
  uint64_t result = 0;
  if (from >= len) {
    return 0;
  }
  for (size_t i = from; i < len; ++i) {
    unsigned digit;
    char c = input[i];
    if (c >= '0' && c <= '9') {
      digit = (unsigned)(c - '0');
    }
    else if (c >= 'a' && c <= 'f') {
      digit = (unsigned)(c - 'a') + 10u;
    }
    else if (c >= 'A' && c <= 'F') {
      digit = (unsigned)(c - 'A') + 10u;
    }
    else {
      return 0;
    }
    if (result > (UINT64_MAX >> 4)) {
      return 0; // Overflow
    }
    result = (result << 4) | digit;
  }
  *out_u64 = result;
  return 1;
}

GTEXT_INTERNAL_API GTEXT_JSON_Status json_parse_number(const char * input,
    size_t input_len, json_number * num, json_position * pos,
    const GTEXT_JSON_Parse_Options * opts) {
  if (!input || !num || input_len == 0) {
    return GTEXT_JSON_E_INVALID;
  }

  // Initialize output structure
  memset(num, 0, sizeof(json_number));
  num->alloc = opts ? opts->allocator : NULL;

  // Check for nonfinite numbers first (always check, but return error if
  // disabled)
  if (json_parse_nonfinite(
          input, input_len, num, opts && opts->allow_leading_plus)) {
    // Found a non-finite number
    if (!opts || !opts->allow_nonfinite_numbers) {
      // Non-finite numbers not allowed - return specific error
      // Note: json_parse_nonfinite doesn't allocate memory, so no cleanup
      // needed
      memset(num, 0, sizeof(json_number));
      return GTEXT_JSON_E_NONFINITE;
    }
    // Non-finite numbers allowed - preserve lexeme if requested
    if (opts->preserve_number_lexeme) {
      // Check for integer overflow
      if (input_len > SIZE_MAX - 1) {
        return GTEXT_JSON_E_LIMIT;
      }
      num->lexeme = gtext_allocator_malloc(num->alloc, input_len + 1);
      if (!num->lexeme) {
        return GTEXT_JSON_E_OOM;
      }
      memcpy(num->lexeme, input, input_len);
      num->lexeme[input_len] = '\0';
      num->lexeme_len = input_len;
      num->flags |= JSON_NUMBER_HAS_LEXEME;
    }
    return GTEXT_JSON_OK;
  }

  // Validate number syntax
  if (!json_validate_number_syntax(input, input_len, 0, opts)) {
    return GTEXT_JSON_E_BAD_NUMBER;
  }

  const size_t hex_from = json_hex_digits_at(input, input_len, opts);

  // Preserve lexeme if requested
  if (opts && opts->preserve_number_lexeme) {
    // Check for integer overflow
    if (input_len > SIZE_MAX - 1) {
      return GTEXT_JSON_E_LIMIT;
    }
    num->lexeme = gtext_allocator_malloc(num->alloc, input_len + 1);
    if (!num->lexeme) {
      return GTEXT_JSON_E_OOM;
    }
    memcpy(num->lexeme, input, input_len);
    num->lexeme[input_len] = '\0';
    num->lexeme_len = input_len;
    num->flags |= JSON_NUMBER_HAS_LEXEME;
  }

  if (hex_from) {
    // A hex literal is an integer by construction, so the integer paths are
    // the authority here and the double is derived from what they found
    // rather than from a second scan of the text.
    uint64_t magnitude;
    if (json_parse_hex_u64(input, input_len, hex_from, &magnitude)) {
      const int negative = input[0] == '-';
      if (opts && opts->parse_uint64 && !negative) {
        num->u64 = magnitude;
        num->flags |= JSON_NUMBER_HAS_U64;
      }
      if (opts && opts->parse_int64) {
        if (negative && magnitude <= (uint64_t)INT64_MAX + 1) {
          num->i64 = magnitude == (uint64_t)INT64_MAX + 1
              ? INT64_MIN
              : -(int64_t)magnitude;
          num->flags |= JSON_NUMBER_HAS_I64;
        }
        else if (!negative && magnitude <= (uint64_t)INT64_MAX) {
          num->i64 = (int64_t)magnitude;
          num->flags |= JSON_NUMBER_HAS_I64;
        }
      }
      if (opts && opts->parse_double) {
        num->dbl = negative ? -(double)magnitude : (double)magnitude;
        num->flags |= JSON_NUMBER_HAS_DOUBLE;
      }
    }
    if (pos) {
      json_position_update_offset(pos, input_len);
      json_position_update_column(pos, input_len);
    }
    return GTEXT_JSON_OK;
  }

  // Parse int64 if requested
  if (opts && opts->parse_int64) {
    int64_t i64_val;
    if (json_parse_int64(input, input_len, &i64_val)) {
      num->i64 = i64_val;
      num->flags |= JSON_NUMBER_HAS_I64;
    }
  }

  // Parse uint64 if requested (only for non-negative numbers)
  if (opts && opts->parse_uint64 && input[0] != '-') {
    uint64_t u64_val;
    if (json_parse_uint64(input, input_len, &u64_val)) {
      num->u64 = u64_val;
      num->flags |= JSON_NUMBER_HAS_U64;
    }
  }

  // Parse double using strtod
  if (opts && opts->parse_double) {
    // Create null-terminated string for strtod
    // Check for integer overflow
    if (input_len > SIZE_MAX - 1) {
      // Clean up lexeme if allocated
      if (num->lexeme) {
        gtext_allocator_free(num->alloc, num->lexeme);
        num->lexeme = NULL;
      }
      return GTEXT_JSON_E_LIMIT;
    }
    char * strtod_input = gtext_allocator_malloc(num->alloc, input_len + 1);
    if (!strtod_input) {
      // Clean up lexeme if allocated
      if (num->lexeme) {
        gtext_allocator_free(num->alloc, num->lexeme);
        num->lexeme = NULL;
      }
      return GTEXT_JSON_E_OOM;
    }
    memcpy(strtod_input, input, input_len);
    strtod_input[input_len] = '\0';

    char * endptr;
    errno = 0;
    /* Not strtod: where LC_NUMERIC's separator is a comma it stops at the
       "." and the "entire string consumed" test below then leaves the
       number with no double value at all. */
    double dbl_val = gtext_number_strtod(num->alloc, strtod_input, &endptr);

    // Check if entire string was consumed
    if (endptr == strtod_input + input_len && errno == 0) {
      // Check for nonfinite numbers (if not already handled)
      if (!isnan(dbl_val) && !isinf(dbl_val)) {
        num->dbl = dbl_val;
        num->flags |= JSON_NUMBER_HAS_DOUBLE;
      }
      else if (opts && opts->allow_nonfinite_numbers) {
        num->dbl = dbl_val;
        num->flags |= JSON_NUMBER_HAS_DOUBLE | JSON_NUMBER_IS_NONFINITE;
      }
    }

    gtext_allocator_free(num->alloc, strtod_input);
  }

  // Update position if provided
  if (pos) {
    // Update offset and column (line doesn't change for numbers)
    json_position_update_offset(pos, input_len);
    json_position_update_column(pos, input_len);
  }

  return GTEXT_JSON_OK;
}

GTEXT_INTERNAL_API void json_number_destroy(json_number * num) {
  if (!num) {
    return;
  }

  // Free the lexeme if it was allocated
  if (num->lexeme) {
    gtext_allocator_free(num->alloc, num->lexeme);
    num->lexeme = NULL;
    num->lexeme_len = 0;
    num->flags &= ~JSON_NUMBER_HAS_LEXEME;
  }
}

/*
 * A JSON number as an exact decimal: mantissa * 10^exponent.
 *
 * JSON numbers are decimal text, and `multipleOf` asks a decimal question -
 * "is 0.0075 a multiple of 0.0001" is asking whether 75 * 0.0001 is 0.0075,
 * which in decimal it plainly is.  Answered in binary it is not, because
 * neither value is exactly representable and the error does not cancel.  So
 * the question is answered over the digits the document actually wrote.
 *
 * Only what fits: a mantissa of more than 19 significant digits, or one that
 * overflows on the way in, reports failure and the caller falls back.  This
 * is not a bignum, and pretending otherwise by silently truncating digits
 * would answer a different question than the one asked.
 */
GTEXT_INTERNAL_API int json_decimal_from_lexeme(
    const char * lexeme, size_t len, json_decimal * out) {
  if (!lexeme || !out || len == 0) {
    return 0;
  }
  size_t i = 0;
  int negative = 0;
  if (lexeme[i] == '-' || lexeme[i] == '+') {
    negative = (lexeme[i] == '-');
    i++;
  }

  uint64_t mantissa = 0;
  int digits = 0;      // significant digits accumulated
  int frac_digits = 0; // digits taken after the decimal point
  int seen_digit = 0;
  int in_fraction = 0;
  for (; i < len; i++) {
    char c = lexeme[i];
    if (c == '.') {
      if (in_fraction) {
        return 0;
      }
      in_fraction = 1;
      continue;
    }
    if (!json_is_digit(c)) {
      break;
    }
    seen_digit = 1;
    if (mantissa == 0 && c == '0') {
      /* A leading zero contributes no significant digit, but a zero after
       * the point still shifts everything right of it. */
      if (in_fraction) {
        frac_digits++;
      }
      continue;
    }
    if (digits >= 19 || mantissa > (UINT64_MAX - 9) / 10) {
      return 0;
    }
    mantissa = mantissa * 10 + (uint64_t)(c - '0');
    digits++;
    if (in_fraction) {
      frac_digits++;
    }
  }
  if (!seen_digit) {
    return 0;
  }

  int64_t exponent = -(int64_t)frac_digits;
  if (i < len && (lexeme[i] == 'e' || lexeme[i] == 'E')) {
    i++;
    int exp_negative = 0;
    if (i < len && (lexeme[i] == '-' || lexeme[i] == '+')) {
      exp_negative = (lexeme[i] == '-');
      i++;
    }
    int64_t value = 0;
    if (i >= len || !json_is_digit(lexeme[i])) {
      return 0;
    }
    for (; i < len && json_is_digit(lexeme[i]); i++) {
      if (value > 1000000) {
        /* Far past any exponent the divisibility loops below will walk;
         * saying so here keeps them bounded. */
        return 0;
      }
      value = value * 10 + (lexeme[i] - '0');
    }
    exponent += exp_negative ? -value : value;
  }
  if (i != len) {
    return 0; // trailing text: not a bare number lexeme
  }

  /* Trailing zeros come off the mantissa and go onto the exponent, which
   * keeps both divisibility loops short for values like 1e8 written out. */
  while (mantissa != 0 && mantissa % 10 == 0) {
    mantissa /= 10;
    exponent += 1;
  }
  if (mantissa == 0) {
    exponent = 0;
  }

  out->mantissa = mantissa;
  out->exponent = exponent;
  out->negative = negative;
  return 1;
}

/*
 * Is `value` an exact integer multiple of `divisor`?
 *
 * Returns 1 for yes, 0 for no, and -1 only when it was handed something it
 * could not read.  A caller that gets -1 has been told nothing and must
 * decide some other way; folding that into "no" would refuse an instance over
 * an implementation limit rather than over the schema.
 *
 * value = Mv * 10^Ev and divisor = Md * 10^Ed, so the question is whether
 * Md divides Mv * 10^(Ev-Ed).  Rather than compute that product - which
 * overflows for any interesting exponent - Md is split into 2^a * 5^b * Q
 * with Q coprime to 10.  The three factors are pairwise coprime, so the
 * product divides exactly when all three do, and 10^gap can only ever help
 * with the first two:
 *
 *   Q divides Mv, and v2(Mv) + gap >= a, and v5(Mv) + gap >= b
 *
 * where v2 and v5 count how many times 2 and 5 divide Mv.  Every term fits in
 * a uint64 no matter how far apart the exponents are, so there is no limit to
 * report and no arithmetic to guard.
 *
 * Sign is irrelevant: -6 is as much a multiple of 3 as 6 is.
 */
static int json_decimal_valuation(uint64_t value, uint64_t prime) {
  int count = 0;
  while (value != 0 && value % prime == 0) {
    value /= prime;
    count++;
  }
  return count;
}

GTEXT_INTERNAL_API int json_decimal_is_multiple_of(
    const json_decimal * value, const json_decimal * divisor) {
  if (!value || !divisor || divisor->mantissa == 0) {
    return -1;
  }
  if (value->mantissa == 0) {
    return 1; // zero is a multiple of everything
  }

  int64_t gap = value->exponent - divisor->exponent;
  int a = json_decimal_valuation(divisor->mantissa, 2);
  int b = json_decimal_valuation(divisor->mantissa, 5);

  uint64_t coprime = divisor->mantissa;
  for (int i = 0; i < a; i++) {
    coprime /= 2;
  }
  for (int i = 0; i < b; i++) {
    coprime /= 5;
  }
  if (value->mantissa % coprime != 0) {
    return 0;
  }
  if ((int64_t)json_decimal_valuation(value->mantissa, 2) + gap < a) {
    return 0;
  }
  if ((int64_t)json_decimal_valuation(value->mantissa, 5) + gap < b) {
    return 0;
  }
  return 1;
}

/*
 * Does a number lexeme denote a whole number?
 *
 * This is asked of the digits the document wrote, not of the double they were
 * converted to, because the two disagree in both directions at the edges. A
 * fifty-three digit integer is not representable as a double and yet it is an
 * integer; `972783798187987123879878123.188781371` is not an integer and yet
 * the nearest double to it is whole. JSON Schema's "integer" is a constraint
 * on the value the document denotes, so the lexeme is the thing to ask.
 *
 * No arithmetic is done on the digits, so there is no size at which this stops
 * working. A JSON number is `D * 10^(e - f)`, where `D` is the digit string
 * with the point taken out, `f` is how many of those digits came after the
 * point and `e` is the exponent. `k = f - e` is how many digits end up to the
 * right of the point, and the question is only whether those digits are all
 * zero.
 */
GTEXT_INTERNAL_API int json_decimal_lexeme_is_integer(
    const char * lexeme, size_t len) {
  if (!lexeme || len == 0) {
    return -1;
  }

  size_t i = 0;
  if (lexeme[i] == '-' || lexeme[i] == '+') {
    i++;
  }

  size_t digits = 0;  // length of D
  size_t zeros = 0;   // trailing zeros of D, counted as it is read
  int any_nonzero = 0;
  size_t fraction = 0; // f

  size_t int_start = i;
  while (i < len && lexeme[i] >= '0' && lexeme[i] <= '9') {
    digits++;
    if (lexeme[i] == '0') {
      zeros++;
    }
    else {
      zeros = 0;
      any_nonzero = 1;
    }
    i++;
  }
  if (i == int_start) {
    return -1; // JSON requires at least one integer digit
  }

  if (i < len && lexeme[i] == '.') {
    i++;
    size_t frac_start = i;
    while (i < len && lexeme[i] >= '0' && lexeme[i] <= '9') {
      digits++;
      fraction++;
      if (lexeme[i] == '0') {
        zeros++;
      }
      else {
        zeros = 0;
        any_nonzero = 1;
      }
      i++;
    }
    if (i == frac_start) {
      return -1;
    }
  }

  /*
   * The exponent is clamped rather than parsed exactly. It is only ever
   * compared against `f`, which is bounded by the length of the lexeme, so any
   * magnitude past that bound gives the same answer as the bound itself - and
   * `1e999999999` must not become an overflowed small number on the way to
   * being told it is an integer.
   */
  long exponent = 0;
  if (i < len && (lexeme[i] == 'e' || lexeme[i] == 'E')) {
    i++;
    int negative = 0;
    if (i < len && (lexeme[i] == '+' || lexeme[i] == '-')) {
      negative = lexeme[i] == '-';
      i++;
    }
    size_t exp_start = i;
    while (i < len && lexeme[i] >= '0' && lexeme[i] <= '9') {
      if (exponent < 1000000) {
        exponent = exponent * 10 + (lexeme[i] - '0');
      }
      i++;
    }
    if (i == exp_start) {
      return -1;
    }
    if (negative) {
      exponent = -exponent;
    }
  }

  if (i != len) {
    return -1; // trailing text: not a bare number lexeme
  }

  if (!any_nonzero) {
    return 1; // every digit is zero, however it is scaled
  }

  long k = (long)fraction - exponent;
  if (k <= 0) {
    return 1;
  }
  if ((unsigned long)k > (unsigned long)digits) {
    /* Scaled below one, and not zero, so there is a digit to the right of the
     * point wherever the significant digits ended up. */
    return 0;
  }
  return (unsigned long)k <= (unsigned long)zeros ? 1 : 0;
}
