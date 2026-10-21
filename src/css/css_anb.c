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
 * The An+B microsyntax: CSS Syntax 6.2 (reading `<an-plus-b>` from tokens)
 * and 9.1 (serializing it).
 *
 * The grammar is matched on tokens exactly as 6.2 writes it, so that the
 * values it reads come from the tokens' values: `-n-\35` is
 * `<dashndashdigit-ident>` (an escape of a digit is a digit by then), and a
 * `+` is a `<delim-token>` only when the tokenizer did not read a number
 * with it. A and B are integers in the standard; here they saturate at the
 * range of int64_t, which the standard does not bound.
 */

#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "css_internal.h"

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static size_t skip_ws(const GTEXT_CSS_Component_Value * v, size_t n, size_t i) {
  while (i < n && is_ws(&v[i])) {
    i++;
  }
  return i;
}

static int64_t saturate(double d) {
  if (d != d) {
    return 0;
  }
  if (d >= 9223372036854775807.0) {
    return INT64_MAX;
  }
  if (d <= -9223372036854775808.0) {
    return INT64_MIN;
  }
  return (int64_t)d;
}

static int64_t negate(int64_t v) {
  return v == INT64_MIN ? INT64_MAX : -v;
}

static bool is_token(const GTEXT_CSS_Component_Value * v, GTEXT_CSS_Token_Type type) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == type;
}

/* An <integer>: a number token with the "integer" type flag. */
static bool is_integer(const GTEXT_CSS_Component_Value * v) {
  return is_token(v, GTEXT_CSS_TOKEN_NUMBER) && (v->flags & GTEXT_CSS_FLAG_INTEGER);
}

static bool has_sign(const GTEXT_CSS_Component_Value * v) {
  return (v->flags & (GTEXT_CSS_FLAG_SIGN_PLUS | GTEXT_CSS_FLAG_SIGN_MINUS)) != 0;
}

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 0x20) : c;
}

/* Whether `s` (length n) is, ASCII case-insensitively, `prefix` followed by
   `digits` (0: nothing after it; 1: one or more digits after it). On a
   match with digits, *value is them (saturating). */
static bool shape(const char * s, size_t n, const char * prefix, bool digits, int64_t * value) {
  const size_t pn = strlen(prefix);
  if (n < pn) {
    return false;
  }
  for (size_t i = 0; i < pn; i++) {
    if (lower(s[i]) != prefix[i]) {
      return false;
    }
  }
  if (!digits) {
    return n == pn;
  }
  if (n == pn) {
    return false;
  }
  int64_t v = 0;
  for (size_t i = pn; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
    const int d = s[i] - '0';
    v = v > (INT64_MAX - d) / 10 ? INT64_MAX : v * 10 + d;
  }
  *value = v;
  return true;
}

bool gtext_css_anb_parse(const GTEXT_CSS_Component_Value * values, size_t count,
    int64_t * a, int64_t * b) {
  if ((!values && count > 0) || !a || !b) {
    return false;
  }
  size_t i = skip_ws(values, count, 0);
  if (i == count) {
    return false;
  }
  int64_t A = 0, B = 0, digits = 0;
  enum { END, BPART, SIGNLESS } next = END;
  const GTEXT_CSS_Component_Value * t = &values[i];
  if (is_token(t, GTEXT_CSS_TOKEN_IDENT)) {
    const char * s = t->value.data;
    const size_t n = t->value.length;
    i++;
    if (shape(s, n, "odd", false, NULL)) {
      A = 2;
      B = 1;
    }
    else if (shape(s, n, "even", false, NULL)) {
      A = 2;
      B = 0;
    }
    else if (shape(s, n, "n", false, NULL)) {
      A = 1;
      next = BPART;
    }
    else if (shape(s, n, "-n", false, NULL)) {
      A = -1;
      next = BPART;
    }
    else if (shape(s, n, "n-", false, NULL)) {
      A = 1;
      next = SIGNLESS;
    }
    else if (shape(s, n, "-n-", false, NULL)) {
      A = -1;
      next = SIGNLESS;
    }
    else if (shape(s, n, "n-", true, &digits)) {
      A = 1;
      B = negate(digits);
    }
    else if (shape(s, n, "-n-", true, &digits)) {
      A = -1;
      B = negate(digits);
    }
    else {
      return false;
    }
  }
  else if (is_token(t, GTEXT_CSS_TOKEN_DELIM) && t->delim == '+') {
    /* '+'?† : no white space between the + and the n */
    i++;
    if (i == count || !is_token(&values[i], GTEXT_CSS_TOKEN_IDENT)) {
      return false;
    }
    const char * s = values[i].value.data;
    const size_t n = values[i].value.length;
    i++;
    if (shape(s, n, "n", false, NULL)) {
      A = 1;
      next = BPART;
    }
    else if (shape(s, n, "n-", false, NULL)) {
      A = 1;
      next = SIGNLESS;
    }
    else if (shape(s, n, "n-", true, &digits)) {
      A = 1;
      B = negate(digits);
    }
    else {
      return false;
    }
  }
  else if (is_integer(t)) {
    A = 0;
    B = saturate(t->number);
    i++;
  }
  else if (is_token(t, GTEXT_CSS_TOKEN_DIMENSION) && (t->flags & GTEXT_CSS_FLAG_INTEGER)) {
    const char * s = t->unit.data;
    const size_t n = t->unit.length;
    A = saturate(t->number);
    i++;
    if (shape(s, n, "n", false, NULL)) {
      next = BPART;
    }
    else if (shape(s, n, "n-", false, NULL)) {
      next = SIGNLESS;
    }
    else if (shape(s, n, "n-", true, &digits)) {
      B = negate(digits);
    }
    else {
      return false;
    }
  }
  else {
    return false;
  }
  if (next == BPART) {
    i = skip_ws(values, count, i);
    if (i < count) {
      t = &values[i];
      if (is_integer(t) && has_sign(t)) {
        B = saturate(t->number);
        i++;
      }
      else if (is_token(t, GTEXT_CSS_TOKEN_DELIM) && (t->delim == '+' || t->delim == '-')) {
        const bool minus = t->delim == '-';
        i = skip_ws(values, count, i + 1);
        if (i == count || !is_integer(&values[i]) || has_sign(&values[i])) {
          return false;
        }
        B = saturate(values[i].number);
        if (minus) {
          B = negate(B);
        }
        i++;
      }
      else {
        return false;
      }
    }
  }
  else if (next == SIGNLESS) {
    i = skip_ws(values, count, i);
    if (i == count || !is_integer(&values[i]) || has_sign(&values[i])) {
      return false;
    }
    B = negate(saturate(values[i].number));
    i++;
  }
  if (skip_ws(values, count, i) != count) {
    return false;
  }
  *a = A;
  *b = B;
  return true;
}

static size_t put_i64(char * out, int64_t v) {
  char tmp[24];
  size_t n = 0;
  uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do {
    tmp[n++] = (char)('0' + (u % 10));
    u /= 10;
  } while (u);
  size_t k = 0;
  if (v < 0) {
    out[k++] = '-';
  }
  while (n) {
    out[k++] = tmp[--n];
  }
  return k;
}

size_t gtext_css_anb_serialize(int64_t a, int64_t b, char * buffer, size_t size) {
  char out[48];
  size_t n = 0;
  if (a == 0) {
    n = put_i64(out, b);
  }
  else {
    if (a == 1) {
      out[n++] = 'n';
    }
    else if (a == -1) {
      out[n++] = '-';
      out[n++] = 'n';
    }
    else {
      n = put_i64(out, a);
      out[n++] = 'n';
    }
    if (b > 0) {
      out[n++] = '+';
      n += put_i64(out + n, b);
    }
    else if (b < 0) {
      n += put_i64(out + n, b);
    }
  }
  if (!buffer || size <= n) {
    return 0;
  }
  memcpy(buffer, out, n);
  buffer[n] = '\0';
  return n;
}
