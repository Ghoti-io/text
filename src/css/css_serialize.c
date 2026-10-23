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
 * The CSS serializer: CSS Syntax section 9, spelled with CSSOM's
 * "Common Serializing Idioms" (2.1 of the CSSOM Editor's Draft of 13 October
 * 2026).
 *
 * What it must do is one thing: write text that parses back to the same
 * tree. Each token is spelled so that the tokenizer reads it back as the
 * same token, and two tokens that would run together are separated by an
 * empty comment, by the table of section 9 and by four more pairs the table
 * does not list (each is commented where it is checked). Rules and
 * declarations are spelled so that the parser reads them back as the same
 * kind of construct in the same place.
 *
 * The walk is iterative, over an explicit stack, so a tree as deep as
 * `max_depth` allowed is written without recursion.
 */

#include <ghoti.io/math/decimal.h>
#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "css_internal.h"

/// The default of max_output_bytes.
#define DEFAULT_MAX_OUTPUT ((size_t)256 * 1024 * 1024)

/* What the last token written was, for the table of section 9 (its rows). */
enum {
  R_NONE = 0,
  R_IDENT,
  R_IDENT_U,     ///< an ident "u" or "U" (one of the extra pairs)
  R_IDENT_DASHES, ///< the ident "--": with a ">" after it, a CDC token
  R_AT_KEYWORD,
  R_HASH,
  R_DIMENSION,
  R_DELIM_HASH,
  R_DELIM_MINUS,
  R_NUMBER,
  R_DELIM_AT,
  R_DELIM_DOT,
  R_DELIM_PLUS,
  R_DELIM_SLASH,
  R_DELIM_LT,    ///< extra
  R_URANGE,      ///< extra
  R_WS,          ///< white space: a second one is not written
  R_OTHER
};

/* What the next token is (the table's columns). */
enum {
  C_IDENT = 1u << 0,
  C_FUNCTION = 1u << 1,
  C_URL = 1u << 2,
  C_BAD_URL = 1u << 3,
  C_MINUS = 1u << 4,
  C_NUMBER = 1u << 5,
  C_PERCENTAGE = 1u << 6,
  C_DIMENSION = 1u << 7,
  C_CDC = 1u << 8,
  C_LPAREN = 1u << 9,
  C_STAR = 1u << 10,
  C_PERCENT = 1u << 11,
  C_BANG = 1u << 12,     ///< extra
  C_PLUS = 1u << 13,     ///< extra
  C_QUESTION = 1u << 14, ///< extra
  C_URANGE = 1u << 15,   ///< extra: a unicode-range token (the token after "U" and "+")
  C_GT = 1u << 16        ///< extra: a ">" delim
};

/// The columns of the names and urls.
#define C_IDENTISH (C_IDENT | C_FUNCTION | C_URL | C_BAD_URL)
/// The columns of the numeric tokens.
#define C_NUMERIC (C_NUMBER | C_PERCENTAGE | C_DIMENSION)

static unsigned needs_comment_before(int row) {
  switch (row) {
  case R_IDENT:
    return C_IDENTISH | C_MINUS | C_NUMERIC | C_CDC | C_LPAREN | C_URANGE;
  case R_IDENT_DASHES:
    /* "--" then ">" is the CDC token "-->": the ident is two hyphens only when it is one token. */
    return C_IDENTISH | C_MINUS | C_NUMERIC | C_CDC | C_LPAREN | C_URANGE | C_GT;
  case R_IDENT_U:
    /* "u" then "+": in a unicode-range value, read again, the two would be
       the start of a unicode-range token. */
    return C_IDENTISH | C_MINUS | C_NUMERIC | C_CDC | C_LPAREN | C_PLUS | C_URANGE;
  case R_AT_KEYWORD:
  case R_HASH:
  case R_DIMENSION:
  case R_DELIM_HASH:
  case R_DELIM_MINUS:
    /* A name or a number runs on into the "U" of a unicode-range token:
       "foo" "U+25" would read back as the ident "fooU" and a number. */
    return C_IDENTISH | C_MINUS | C_NUMERIC | C_CDC | C_URANGE;
  case R_NUMBER:
    return C_IDENTISH | C_NUMERIC | C_CDC | C_PERCENT | C_URANGE;
  case R_DELIM_AT:
    return C_IDENTISH | C_MINUS | C_CDC | C_URANGE;
  case R_DELIM_DOT:
  case R_DELIM_PLUS:
    return C_NUMERIC;
  case R_DELIM_SLASH:
    return C_STAR;
  case R_DELIM_LT:
    /* "<" then "!": with "--" after them they would be a CDO token. */
    return C_BANG;
  case R_URANGE:
    /* A unicode-range token takes hex digits, "?" and "-hex" after it. */
    return C_IDENTISH | C_NUMERIC | C_MINUS | C_QUESTION | C_CDC;
  default:
    return 0;
  }
}

static unsigned column_of(const GTEXT_CSS_Component_Value * v) {
  if (v->kind == GTEXT_CSS_VALUE_FUNCTION) {
    return C_FUNCTION;
  }
  switch (v->type) {
  case GTEXT_CSS_TOKEN_IDENT: return C_IDENT;
  case GTEXT_CSS_TOKEN_URL: return C_URL;
  case GTEXT_CSS_TOKEN_BAD_URL: return C_BAD_URL;
  case GTEXT_CSS_TOKEN_NUMBER: return C_NUMBER;
  case GTEXT_CSS_TOKEN_PERCENTAGE: return C_PERCENTAGE;
  case GTEXT_CSS_TOKEN_DIMENSION: return C_DIMENSION;
  case GTEXT_CSS_TOKEN_CDC: return C_CDC;
  case GTEXT_CSS_TOKEN_UNICODE_RANGE: return C_URANGE;
  case GTEXT_CSS_TOKEN_OPEN_PAREN: return v->kind == GTEXT_CSS_VALUE_BLOCK ? C_LPAREN : 0;
  case GTEXT_CSS_TOKEN_DELIM:
    switch (v->delim) {
    case '-': return C_MINUS;
    case '*': return C_STAR;
    case '%': return C_PERCENT;
    case '!': return C_BANG;
    case '+': return C_PLUS;
    case '?': return C_QUESTION;
    case '>': return C_GT;
    default: return 0;
    }
  default:
    return 0;
  }
}

static int row_of(const GTEXT_CSS_Component_Value * v) {
  if (v->kind != GTEXT_CSS_VALUE_TOKEN) {
    return R_OTHER; /* a function or a block ends with its closing bracket */
  }
  switch (v->type) {
  case GTEXT_CSS_TOKEN_IDENT:
    if (v->value.length == 2 && v->value.data[0] == '-' && v->value.data[1] == '-') {
      return R_IDENT_DASHES;
    }
    return (v->value.length == 1 && (v->value.data[0] == 'u' || v->value.data[0] == 'U')) ? R_IDENT_U : R_IDENT;
  case GTEXT_CSS_TOKEN_AT_KEYWORD: return R_AT_KEYWORD;
  case GTEXT_CSS_TOKEN_HASH: return R_HASH;
  case GTEXT_CSS_TOKEN_DIMENSION: return R_DIMENSION;
  case GTEXT_CSS_TOKEN_NUMBER: return R_NUMBER;
  case GTEXT_CSS_TOKEN_UNICODE_RANGE: return R_URANGE;
  case GTEXT_CSS_TOKEN_WHITESPACE: return R_WS;
  case GTEXT_CSS_TOKEN_DELIM:
    switch (v->delim) {
    case '#': return R_DELIM_HASH;
    case '-': return R_DELIM_MINUS;
    case '@': return R_DELIM_AT;
    case '.': return R_DELIM_DOT;
    case '+': return R_DELIM_PLUS;
    case '/': return R_DELIM_SLASH;
    case '<': return R_DELIM_LT;
    default: return R_OTHER;
    }
  default:
    return R_OTHER;
  }
}

/* ---- the writer ------------------------------------------------------- */

/// The writer: where the text goes and what was last written.
typedef struct {
  const GTEXT_Markup_Sink * sink; ///< Where the text goes.
  GTEXT_CSS_Number_Style numbers; ///< How numbers are spelled.
  size_t max;                     ///< max_output_bytes.
  size_t written;                 ///< Bytes written.
  GTEXT_CSS_Status status;        ///< OK until a write fails.
  int last;                       ///< R_*: the last token's row of the table.
  GTEXT_Allocator alloc;          ///< For the task stack.
  size_t work; ///< bytes written and tasks run (gtext_css_serialize_work())
} writer;

/* Every call is one unit that is whole: a code point, an escape, a keyword, a
   spelling. One that does not fit within the limit is not written at all, so
   the sink has had a prefix of the text that ends between units - never half
   of a multi-byte code point or half of an escape - and the status is
   GTEXT_CSS_E_LIMIT_OUTPUT. */
static void put(writer * w, const char * s, size_t n) {
  w->work += n;
  if (w->status != GTEXT_CSS_OK || n == 0) {
    return;
  }
  if (n > w->max - w->written) {
    w->status = GTEXT_CSS_E_LIMIT_OUTPUT;
    return;
  }
  if (w->sink->write(w->sink->user, s, n) != GTEXT_MARKUP_OK) {
    w->status = GTEXT_CSS_E_WRITE;
    return;
  }
  w->written += n;
}

static void puts_(writer * w, const char * s) {
  put(w, s, strlen(s));
}

/* Text that is not a token the table knows (punctuation, a closing bracket). */
static void put_other(writer * w, const char * s) {
  puts_(w, s);
  w->last = R_OTHER;
}

static void begin(writer * w, unsigned column) {
  if (needs_comment_before(w->last) & column) {
    puts_(w, "/**/");
  }
}

/* Decode one code point of well-formed-or-not UTF-8 (ill-formed: U+FFFD). */
static uint32_t next_cp(const unsigned char * s, size_t n, size_t * i) {
  const unsigned char b = s[*i];
  if (b < 0x80) {
    (*i)++;
    return b;
  }
  size_t need;
  uint32_t v;
  unsigned char lo = 0x80, hi = 0xBF;
  if (b >= 0xC2 && b <= 0xDF) {
    need = 1;
    v = b & 0x1Fu;
  }
  else if (b >= 0xE0 && b <= 0xEF) {
    need = 2;
    v = b & 0x0Fu;
    if (b == 0xE0) {
      lo = 0xA0;
    }
    else if (b == 0xED) {
      hi = 0x9F;
    }
  }
  else if (b >= 0xF0 && b <= 0xF4) {
    need = 3;
    v = b & 0x07u;
    if (b == 0xF0) {
      lo = 0x90;
    }
    else if (b == 0xF4) {
      hi = 0x8F;
    }
  }
  else {
    (*i)++;
    return 0xFFFD;
  }
  for (size_t k = 1; k <= need; k++) {
    if (*i + k >= n || s[*i + k] < lo || s[*i + k] > hi) {
      *i += k;
      return 0xFFFD;
    }
    lo = 0x80;
    hi = 0xBF;
    v = (v << 6) | (s[*i + k] & 0x3Fu);
  }
  *i += need + 1;
  return v;
}

/* UTF-8 for `cp` into b (4 bytes); the length. */
static size_t encode_cp(char * b, uint32_t cp) {
  if (cp < 0x80) {
    b[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    b[0] = (char)(0xC0 | (cp >> 6));
    b[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    b[0] = (char)(0xE0 | (cp >> 12));
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  b[0] = (char)(0xF0 | (cp >> 18));
  b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  b[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static void put_cp(writer * w, uint32_t cp) {
  char b[4];
  put(w, b, encode_cp(b, cp));
}

/* CSSOM: "escape a character as code point". */
static void put_cp_escape(writer * w, uint32_t cp) {
  static const char hex[] = "0123456789abcdef";
  char b[10];
  size_t n = 0;
  b[n++] = '\\';
  char d[8];
  size_t k = 0;
  do {
    d[k++] = hex[cp & 0xF];
    cp >>= 4;
  } while (cp);
  while (k) {
    b[n++] = d[--k];
  }
  b[n++] = ' ';
  put(w, b, n);
}

/* CSSOM: "escape a character". */
static void put_escaped(writer * w, uint32_t cp) {
  char b[5];
  b[0] = '\\';
  put(w, b, 1 + encode_cp(b + 1, cp)); /* one unit: a backslash alone is not a prefix of the text */
}

static bool is_ascii_ident(uint32_t c) {
  return c == '-' || c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

enum {
  ID_PLAIN = 0,
  ID_UNIT = 1,         ///< a dimension's unit: an `e` that would read as an exponent is escaped
  ID_UNRESTRICTED = 2  ///< an unrestricted hash's name: no first-character rules
};

/* CSSOM's "serialize an identifier", with the two variants above. */
static void put_ident(writer * w, const char * text, size_t len, int mode) {
  const unsigned char * s = (const unsigned char *)text;
  size_t i = 0;
  size_t index = 0;
  uint32_t first = 0;
  while (i < len) {
    const uint32_t c = next_cp(s, len, &i);
    if (index == 0) {
      first = c;
    }
    if (c == 0) {
      put_cp(w, 0xFFFD);
    }
    else if ((c >= 1 && c <= 0x1F) || c == 0x7F) {
      put_cp_escape(w, c);
    }
    else if (mode != ID_UNRESTRICTED && index == 0 && c >= '0' && c <= '9') {
      put_cp_escape(w, c);
    }
    else if (mode != ID_UNRESTRICTED && index == 1 && c >= '0' && c <= '9' && first == '-') {
      put_cp_escape(w, c);
    }
    else if (mode != ID_UNRESTRICTED && index == 0 && c == '-' && i >= len) {
      put_escaped(w, c);
    }
    else if (mode == ID_UNIT && index == 0 && (c == 'e' || c == 'E')) {
      /* "The unit of a dimension-token may need escaping to disambiguate with
         scientific notation" (section 9): 1 followed by "e5" or "e-5". */
      const unsigned char n1 = i < len ? s[i] : 0;
      const unsigned char n2 = i + 1 < len ? s[i + 1] : 0;
      if ((n1 >= '0' && n1 <= '9') || ((n1 == '+' || n1 == '-') && n2 >= '0' && n2 <= '9')) {
        put_cp_escape(w, c);
      }
      else {
        put_cp(w, c);
      }
    }
    else if (is_ascii_ident(c) || (c >= 0x80 && css_is_non_ascii_ident(c))) {
      put_cp(w, c);
    }
    else {
      put_escaped(w, c);
    }
    index++;
  }
}

/* CSSOM's "serialize a string". */
static void put_string(writer * w, const char * text, size_t len) {
  const unsigned char * s = (const unsigned char *)text;
  put(w, "\"", 1);
  size_t i = 0;
  while (i < len) {
    const uint32_t c = next_cp(s, len, &i);
    if (c == 0) {
      put_cp(w, 0xFFFD);
    }
    else if ((c >= 1 && c <= 0x1F) || c == 0x7F) {
      put_cp_escape(w, c);
    }
    else if (c == '"' || c == '\\') {
      put_escaped(w, c);
    }
    else {
      put_cp(w, c);
    }
  }
  put(w, "\"", 1);
}

/* An unquoted url token: everything the tokenizer would stop at is escaped.
   (CSSOM's "serialize a URL" writes url("..."), which reads back as a
   function with a string in it: a different tree.) */
static void put_url(writer * w, const char * text, size_t len) {
  const unsigned char * s = (const unsigned char *)text;
  put(w, "url(", 4);
  size_t i = 0;
  while (i < len) {
    const uint32_t c = next_cp(s, len, &i);
    if (c == '"' || c == '\'' || c == '(' || c == ')' || c == '\\') {
      put_escaped(w, c);
    }
    else if (c <= 0x20 || c == 0x7F) {
      /* White space and the non-printable code points end an unquoted url; so do CR and FF, which an
         escape can put in a value and which preprocessing would read back as a newline. */
      put_cp_escape(w, c == 0 ? 0xFFFD : c);
    }
    else {
      put_cp(w, c);
    }
  }
  put(w, ")", 1);
}

/* CSSOM's <number> and <integer> (6.7.2): "a base-ten number using digits 0-9 in the
   shortest form possible, using "." to separate decimals (if any), rounding the value if
   necessary to not produce more than 6 decimals, preceded by "-" if it is negative", with
   "scientific notation is not used". The digits are the math library's shortest decimal
   that rounds back to the value, laid out here; where there are more than six decimals the
   spelling is rounded to six, half away from zero, on those digits. That is a rule of this
   library's and not of CSSOM's, which says nothing about a tie, and it is the same on every
   host because it reads no host: no printf, no locale, no rounding mode.

   The two spellings CSSOM does not give: a NaN (no token has one) is "0", and an infinity
   (a token whose magnitude overflowed a double) is the largest finite double, so that a
   number is always digits. */
enum { CSSOM_MAX_DIGITS = 17, CSSOM_MAX_TEXT = 330 };

/* The bound on the spelling, which the buffer below and the copies into it rest on. The shortest digits of a double are
   at most CSSOM_MAX_DIGITS (17). A value is below 1.8e308, so an integer spelling has at most 309 digits (DBL_MAX is 17
   digits and 292 zeros), and a sign makes it 310. A fraction keeps at most six decimals, so it is a sign, at most 17
   digits and a point, or "0." and at most five zeros before the digits: 26 at most. Rounding up can add one digit
   (99.9 -> 100), which digits[] has room for (CSSOM_MAX_DIGITS + 2). Nothing is ever cut short: the guard on the zeros
   below is a guard and not a truncation, and testCssNumbers writes DBL_MAX, DBL_TRUE_MIN and 1e-300. */
_Static_assert(CSSOM_MAX_TEXT >= 1 + 309, "an integer below 1.8e308 and its sign fit the CSSOM buffer");
_Static_assert(CSSOM_MAX_TEXT >= 1 + CSSOM_MAX_DIGITS + 1 + 8, "a fraction fits the CSSOM buffer");

size_t css_cssom_number(double v, bool integer, char * out) {
  if (v != v) {
    out[0] = '0';
    return 1;
  }
  if (v == 1.0 / 0.0 || v == -1.0 / 0.0) {
    v = v > 0 ? 1.7976931348623157e308 : -1.7976931348623157e308;
  }
  GMATH_Decimal d;
  if (gmath_decimal_shortest(v, &d) != GMATH_OK) {
    out[0] = '0';
    return 1;
  }
  char digits[CSSOM_MAX_DIGITS + 2];
  size_t n = d.length;
  memcpy(digits, d.digits, n);
  int64_t tens = d.exponent; /* the value is the digits times ten to this */
  const int64_t keep = integer ? 0 : 6; /* decimals allowed */
  if (-tens > keep) {
    const int64_t drop = -tens - keep; /* digits to remove from the right */
    if (drop > (int64_t)n) { /* below half of the last place kept */
      digits[0] = '0';
      n = 1;
      tens = 0;
    }
    else {
      const bool up = digits[n - (size_t)drop] >= '5'; /* the first digit removed */
      n -= (size_t)drop;
      tens += drop;
      if (up) {
        size_t k = n;
        while (k > 0 && digits[k - 1] == '9') {
          digits[--k] = '0';
        }
        if (k == 0) { /* 99.9 -> 100, or nothing left: a new leading digit */
          memmove(digits + 1, digits, n);
          digits[0] = '1';
          n++;
        }
        else {
          digits[k - 1]++;
        }
      }
      if (n == 0) { /* every digit was removed and the rounding did not go up */
        digits[0] = '0';
        n = 1;
        tens = 0;
      }
    }
  }
  /* Strip zeros made by the rounding (or the shortest form's own) off the right of the fraction. */
  while (n > 1 && digits[n - 1] == '0' && tens < 0) {
    n--;
    tens++;
  }
  bool zero = true;
  for (size_t i = 0; i < n; i++) {
    if (digits[i] != '0') {
      zero = false;
    }
  }
  size_t len = 0;
  if (zero) {
    out[0] = '0';
    return 1;
  }
  if (d.negative) {
    out[len++] = '-';
  }
  if (tens >= 0) { /* an integer: the digits and their zeros */
    memcpy(out + len, digits, n);
    len += n;
    for (int64_t i = 0; i < tens && len < CSSOM_MAX_TEXT; i++) {
      out[len++] = '0';
    }
  }
  else if ((int64_t)n + tens > 0) { /* digits before and after the point */
    const size_t before = (size_t)((int64_t)n + tens);
    memcpy(out + len, digits, before);
    len += before;
    out[len++] = '.';
    memcpy(out + len, digits + before, n - before);
    len += n - before;
  }
  else { /* 0.000ddd */
    out[len++] = '0';
    out[len++] = '.';
    for (int64_t i = 0; i < -((int64_t)n + tens); i++) {
      out[len++] = '0';
    }
    memcpy(out + len, digits, n);
    len += n;
  }
  return len;
}

static void put_cssom_number(writer * w, double v, bool integer) {
  char out[CSSOM_MAX_TEXT];
  put(w, out, css_cssom_number(v, integer, out));
}

static void put_number(writer * w, const GTEXT_CSS_Component_Value * v) {
  if (w->numbers == GTEXT_CSS_NUMBERS_CSSOM || !v->repr.data) {
    const bool integer = v->type != GTEXT_CSS_TOKEN_PERCENTAGE && (v->flags & GTEXT_CSS_FLAG_INTEGER);
    put_cssom_number(w, v->number, integer);
    return;
  }
  put(w, v->repr.data, v->repr.length);
}

static void put_hex(writer * w, uint32_t v) {
  static const char hex[] = "0123456789ABCDEF";
  char d[8];
  size_t k = 0;
  do {
    d[k++] = hex[v & 0xF];
    v >>= 4;
  } while (v);
  char b[8];
  size_t n = 0;
  while (k) {
    b[n++] = d[--k];
  }
  put(w, b, n);
}

/* One preserved token, or the opening token of a function or a block. */
static void put_token(writer * w, const GTEXT_CSS_Component_Value * v) {
  if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE && w->last == R_WS) {
    /* Two whitespace tokens in a row (a comment was between them) are written as one, which
       section 9 allows and which makes a second serialization the same text as the first. */
    return;
  }
  begin(w, column_of(v));
  if (v->kind == GTEXT_CSS_VALUE_FUNCTION) {
    put_ident(w, v->value.data, v->value.length, ID_PLAIN);
    put(w, "(", 1);
    w->last = R_OTHER;
    return;
  }
  if (v->kind == GTEXT_CSS_VALUE_BLOCK) {
    put(w, v->type == GTEXT_CSS_TOKEN_OPEN_SQUARE ? "[" : v->type == GTEXT_CSS_TOKEN_OPEN_CURLY ? "{" : "(", 1);
    w->last = R_OTHER;
    return;
  }
  switch (v->type) {
  case GTEXT_CSS_TOKEN_IDENT:
    put_ident(w, v->value.data, v->value.length, ID_PLAIN);
    break;
  case GTEXT_CSS_TOKEN_AT_KEYWORD:
    put(w, "@", 1);
    put_ident(w, v->value.data, v->value.length, ID_PLAIN);
    break;
  case GTEXT_CSS_TOKEN_HASH:
    put(w, "#", 1);
    put_ident(w, v->value.data, v->value.length, (v->flags & GTEXT_CSS_FLAG_HASH_ID) ? ID_PLAIN : ID_UNRESTRICTED);
    break;
  case GTEXT_CSS_TOKEN_STRING:
    put_string(w, v->value.data, v->value.length);
    break;
  case GTEXT_CSS_TOKEN_BAD_STRING:
    /* A quote and a newline is a bad-string token; the newline is then white
       space, which is what always follows a bad-string token. */
    put(w, "\"\n", 2);
    break;
  case GTEXT_CSS_TOKEN_URL:
    put_url(w, v->value.data, v->value.length);
    break;
  case GTEXT_CSS_TOKEN_BAD_URL:
    put(w, "url(()", 6);
    break;
  case GTEXT_CSS_TOKEN_DELIM:
    if (v->delim == '\\') {
      put(w, "\\\n", 2); /* section 9 */
    }
    else {
      put_cp(w, v->delim);
    }
    break;
  case GTEXT_CSS_TOKEN_NUMBER:
    put_number(w, v);
    break;
  case GTEXT_CSS_TOKEN_PERCENTAGE:
    put_number(w, v);
    put(w, "%", 1);
    break;
  case GTEXT_CSS_TOKEN_DIMENSION:
    put_number(w, v);
    put_ident(w, v->unit.data, v->unit.length, ID_UNIT);
    break;
  case GTEXT_CSS_TOKEN_UNICODE_RANGE:
    put(w, "U+", 2);
    put_hex(w, v->range_start);
    if (v->range_end != v->range_start) {
      put(w, "-", 1);
      put_hex(w, v->range_end);
    }
    break;
  case GTEXT_CSS_TOKEN_WHITESPACE: put(w, " ", 1); break;
  case GTEXT_CSS_TOKEN_CDO: put(w, "<!--", 4); break;
  case GTEXT_CSS_TOKEN_CDC: put(w, "-->", 3); break;
  case GTEXT_CSS_TOKEN_COLON: put(w, ":", 1); break;
  case GTEXT_CSS_TOKEN_SEMICOLON: put(w, ";", 1); break;
  case GTEXT_CSS_TOKEN_COMMA: put(w, ",", 1); break;
  case GTEXT_CSS_TOKEN_OPEN_SQUARE: put(w, "[", 1); break;
  case GTEXT_CSS_TOKEN_CLOSE_SQUARE: put(w, "]", 1); break;
  case GTEXT_CSS_TOKEN_OPEN_PAREN: put(w, "(", 1); break;
  case GTEXT_CSS_TOKEN_CLOSE_PAREN: put(w, ")", 1); break;
  case GTEXT_CSS_TOKEN_OPEN_CURLY: put(w, "{", 1); break;
  case GTEXT_CSS_TOKEN_CLOSE_CURLY: put(w, "}", 1); break;
  default: break;
  }
  w->last = row_of(v);
}

/* ---- the walk --------------------------------------------------------- */

enum { K_VALUES, K_TEXT, K_RULES, K_DECLS, K_ITEMS, K_GROUPS };

/// A task of the walk: an array to write, or text.
typedef struct {
  int kind;           ///< K_*.
  const void * ptr;   ///< The array.
  size_t n;           ///< Its length.
  size_t i;           ///< The next element.
  const char * text;  ///< K_TEXT
  bool nested;        ///< K_RULES, K_ITEMS: read back inside a block
  bool semicolons;    ///< K_DECLS: each declaration ends with ";"
} task;

/// The walk's stack.
typedef struct {
  task * data; ///< The tasks.
  size_t len;  ///< In use.
  size_t cap;  ///< Allocated.
} taskstack;

static bool tpush(writer * w, taskstack * st, task t) {
  if (st->len == st->cap) {
    const size_t cap = st->cap ? st->cap * 2 : 32;
    if (cap > SIZE_MAX / sizeof(task)) {
      w->status = GTEXT_CSS_E_OOM;
      return false;
    }
    task * grown = gtext_allocator_realloc(&w->alloc, st->data, cap * sizeof(task));
    if (!grown) {
      /* The writer's only allocation; a sink write has no status for memory. */
      w->status = GTEXT_CSS_E_OOM;
      return false;
    }
    st->data = grown;
    st->cap = cap;
  }
  st->data[st->len++] = t;
  return true;
}

static task mk(int kind, const void * ptr, size_t n) {
  task t;
  memset(&t, 0, sizeof t);
  t.kind = kind;
  t.ptr = ptr;
  t.n = n;
  return t;
}

static task mk_text(const char * text) {
  task t = mk(K_TEXT, NULL, 0);
  t.text = text;
  return t;
}

static bool is_ws_value(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

/* A qualified rule whose prelude is an ident, a colon and white space reads
   back, inside a block, as a declaration whose value is the block, if
   nothing follows it there. */
static bool reads_as_declaration(const GTEXT_CSS_Rule * r) {
  if (r->kind != GTEXT_CSS_RULE_QUALIFIED) {
    return false;
  }
  size_t k = 0;
  while (k < r->prelude_count && is_ws_value(&r->prelude[k])) {
    k++;
  }
  if (k == r->prelude_count || r->prelude[k].kind != GTEXT_CSS_VALUE_TOKEN
      || r->prelude[k].type != GTEXT_CSS_TOKEN_IDENT) {
    return false;
  }
  k++;
  while (k < r->prelude_count && is_ws_value(&r->prelude[k])) {
    k++;
  }
  if (k == r->prelude_count || r->prelude[k].kind != GTEXT_CSS_VALUE_TOKEN
      || r->prelude[k].type != GTEXT_CSS_TOKEN_COLON) {
    return false;
  }
  for (k++; k < r->prelude_count; k++) {
    if (!is_ws_value(&r->prelude[k])) {
      return false;
    }
  }
  return true;
}

/* Push the tasks of one rule. `last` says nothing follows it in its block. */
static void rule_tasks(writer * w, taskstack * st, const GTEXT_CSS_Rule * r, bool nested, bool last) {
  if (nested && last && reads_as_declaration(r)) {
    /* "!" makes the value two things, so the declaration fails and the rule
       stands; the "!" is then read as a declaration that fails and as a rule
       with no block, both dropped. Nothing is added to the tree. */
    (void)tpush(w, st, mk_text("!"));
  }
  switch (r->kind) {
  case GTEXT_CSS_RULE_NESTED_DECLARATIONS: {
    task t = mk(K_DECLS, r->declarations, r->declaration_count);
    t.semicolons = true;
    (void)tpush(w, st, t);
    return;
  }
  case GTEXT_CSS_RULE_QUALIFIED: {
    (void)tpush(w, st, mk_text("}"));
    task rules = mk(K_RULES, r->rules, r->rule_count);
    rules.nested = true;
    (void)tpush(w, st, rules);
    task decls = mk(K_DECLS, r->declarations, r->declaration_count);
    decls.semicolons = true;
    (void)tpush(w, st, decls);
    (void)tpush(w, st, mk_text("{"));
    (void)tpush(w, st, mk(K_VALUES, r->prelude, r->prelude_count));
    return;
  }
  default: {
    if (r->has_block) {
      (void)tpush(w, st, mk_text("}"));
      task items = mk(K_ITEMS, r->contents, r->content_count);
      items.nested = true;
      (void)tpush(w, st, items);
      (void)tpush(w, st, mk_text("{"));
    }
    else {
      (void)tpush(w, st, mk_text(";"));
    }
    (void)tpush(w, st, mk(K_VALUES, r->prelude, r->prelude_count));
    GTEXT_CSS_Component_Value kw;
    memset(&kw, 0, sizeof kw);
    kw.kind = GTEXT_CSS_VALUE_TOKEN;
    kw.type = GTEXT_CSS_TOKEN_AT_KEYWORD;
    kw.value = r->name;
    put_token(w, &kw);
    return;
  }
  }
}

/* The value of a unicode-range declaration already holds its "!important"
   tokens (it is the source read again), so the flag is not written twice. */
static void decl(writer * w, taskstack * st, const GTEXT_CSS_Declaration * d, bool semicolon) {
  GTEXT_CSS_Component_Value name;
  memset(&name, 0, sizeof name);
  name.kind = GTEXT_CSS_VALUE_TOKEN;
  name.type = GTEXT_CSS_TOKEN_IDENT;
  name.value = d->name;
  put_token(w, &name);
  put_other(w, ":");
  if (semicolon) {
    (void)tpush(w, st, mk_text(";"));
  }
  if (d->important && !css_ascii_ieq(&d->name, "unicode-range")) {
    (void)tpush(w, st, mk_text(" !important"));
  }
  (void)tpush(w, st, mk(K_VALUES, d->value, d->value_count));
}

static void run(writer * w, taskstack * st) {
  while (st->len > 0 && w->status == GTEXT_CSS_OK) {
    w->work++;
    task * t = &st->data[st->len - 1];
    if (t->kind == K_TEXT) {
      const char * text = t->text;
      st->len--;
      put_other(w, text);
      continue;
    }
    if (t->i >= t->n) {
      st->len--;
      continue;
    }
    const size_t i = t->i++;
    const task cur = *t; /* t may move when something is pushed */
    switch (cur.kind) {
    case K_VALUES: {
      const GTEXT_CSS_Component_Value * v = &((const GTEXT_CSS_Component_Value *)cur.ptr)[i];
      put_token(w, v);
      if (v->kind != GTEXT_CSS_VALUE_TOKEN) {
        const char * close = v->kind == GTEXT_CSS_VALUE_FUNCTION ? ")"
            : v->type == GTEXT_CSS_TOKEN_OPEN_SQUARE           ? "]"
            : v->type == GTEXT_CSS_TOKEN_OPEN_CURLY            ? "}"
                                                               : ")";
        (void)tpush(w, st, mk_text(close));
        (void)tpush(w, st, mk(K_VALUES, v->children, v->child_count));
      }
      break;
    }
    case K_RULES:
      rule_tasks(w, st, &((const GTEXT_CSS_Rule *)cur.ptr)[i], cur.nested, i + 1 == cur.n);
      break;
    case K_ITEMS: {
      const GTEXT_CSS_Block_Item * it = &((const GTEXT_CSS_Block_Item *)cur.ptr)[i];
      if (it->kind == GTEXT_CSS_ITEM_RULE) {
        rule_tasks(w, st, it->rule, cur.nested, i + 1 == cur.n);
      }
      else {
        task d = mk(K_DECLS, it->declarations, it->declaration_count);
        d.semicolons = true;
        (void)tpush(w, st, d);
      }
      break;
    }
    case K_DECLS:
      decl(w, st, &((const GTEXT_CSS_Declaration *)cur.ptr)[i], cur.semicolons);
      break;
    default: {
      const GTEXT_CSS_Value_List * l = &((const GTEXT_CSS_Value_List *)cur.ptr)[i];
      /* Groups are joined with commas; an empty last group was ended by a
         comma of its own, so it gets one. */
      if (i + 1 == cur.n && l->count == 0) {
        (void)tpush(w, st, mk_text(","));
      }
      (void)tpush(w, st, mk(K_VALUES, l->values, l->count));
      if (i > 0) {
        put_other(w, ",");
      }
      break;
    }
    }
  }
}

static bool writer_init(writer * w, const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink) {
  memset(w, 0, sizeof *w);
  if (!sink || !sink->write) {
    return false;
  }
  w->sink = sink;
  w->status = GTEXT_CSS_OK;
  w->last = R_NONE;
  w->numbers = options ? options->numbers : GTEXT_CSS_NUMBERS_SOURCE;
  w->max = options && options->max_output_bytes ? options->max_output_bytes : DEFAULT_MAX_OUTPUT;
  w->alloc = *(options && options->allocator ? options->allocator : gtext_allocator_default());
  return w->numbers == GTEXT_CSS_NUMBERS_SOURCE || w->numbers == GTEXT_CSS_NUMBERS_CSSOM;
}

static GTEXT_CSS_Status finish(writer * w, taskstack * st) {
  run(w, st);
  gtext_allocator_free(&w->alloc, st->data);
  return w->status;
}

GTEXT_CSS_Serialize_Options gtext_css_serialize_options_default(void) {
  GTEXT_CSS_Serialize_Options o;
  memset(&o, 0, sizeof o);
  o.numbers = GTEXT_CSS_NUMBERS_SOURCE;
  o.max_output_bytes = DEFAULT_MAX_OUTPUT;
  return o;
}

static GTEXT_CSS_Status serialize_tree(const GTEXT_CSS_Tree * tree,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink, size_t * work) {
  writer w;
  if (work) {
    *work = 0;
  }
  if (!tree || !writer_init(&w, options, sink)) {
    return GTEXT_CSS_E_INVALID;
  }
  taskstack st = {NULL, 0, 0};
  switch (tree->kind) {
  case GTEXT_CSS_TREE_STYLESHEET:
  case GTEXT_CSS_TREE_STYLESHEET_CONTENTS:
  case GTEXT_CSS_TREE_RULE:
    (void)tpush(&w, &st, mk(K_RULES, tree->rules, tree->rule_count));
    break;
  case GTEXT_CSS_TREE_DECLARATION_LIST: {
    task t = mk(K_DECLS, tree->declarations, tree->declaration_count);
    t.semicolons = true;
    (void)tpush(&w, &st, t);
    break;
  }
  case GTEXT_CSS_TREE_DECLARATION:
    (void)tpush(&w, &st, mk(K_DECLS, tree->declarations, tree->declaration_count));
    break;
  case GTEXT_CSS_TREE_BLOCK_CONTENTS: {
    task t = mk(K_ITEMS, tree->items, tree->item_count);
    t.nested = true;
    (void)tpush(&w, &st, t);
    break;
  }
  case GTEXT_CSS_TREE_COMPONENT_VALUE:
  case GTEXT_CSS_TREE_COMPONENT_VALUE_LIST:
    (void)tpush(&w, &st, mk(K_VALUES, tree->values, tree->value_count));
    break;
  case GTEXT_CSS_TREE_COMMA_LIST:
    (void)tpush(&w, &st, mk(K_GROUPS, tree->lists, tree->list_count));
    break;
  default:
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Status status = finish(&w, &st);
  if (work) {
    *work = w.work;
  }
  return status;
}

GTEXT_CSS_Status gtext_css_serialize_tree(const GTEXT_CSS_Tree * tree,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink) {
  return serialize_tree(tree, options, sink, NULL);
}

/** The quadratic suite's view of the serializer (css_internal.h). */
GTEXT_CSS_Status gtext_css_serialize_work(const GTEXT_CSS_Tree * tree,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink, size_t * work) {
  return serialize_tree(tree, options, sink, work);
}

GTEXT_CSS_Status gtext_css_serialize_rule(const GTEXT_CSS_Rule * rule, bool nested,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink) {
  writer w;
  if (!rule || !writer_init(&w, options, sink)) {
    return GTEXT_CSS_E_INVALID;
  }
  taskstack st = {NULL, 0, 0};
  task t = mk(K_RULES, rule, 1);
  t.nested = nested;
  (void)tpush(&w, &st, t);
  return finish(&w, &st);
}

GTEXT_CSS_Status gtext_css_serialize_declaration(const GTEXT_CSS_Declaration * declaration,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink) {
  writer w;
  if (!declaration || !writer_init(&w, options, sink)) {
    return GTEXT_CSS_E_INVALID;
  }
  taskstack st = {NULL, 0, 0};
  (void)tpush(&w, &st, mk(K_DECLS, declaration, 1));
  return finish(&w, &st);
}

GTEXT_CSS_Status gtext_css_serialize_values(const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink) {
  writer w;
  if ((!values && count > 0) || !writer_init(&w, options, sink)) {
    return GTEXT_CSS_E_INVALID;
  }
  taskstack st = {NULL, 0, 0};
  (void)tpush(&w, &st, mk(K_VALUES, values, count));
  return finish(&w, &st);
}

/* ---- buffers ---------------------------------------------------------- */

/// The sink of the buffer forms.
typedef struct {
  GTEXT_Allocator alloc; ///< The options' allocator.
  char * data;           ///< The text.
  size_t len;            ///< Its length.
  size_t cap;            ///< Allocated.
  bool oom;              ///< An allocation failed.
} membuf;

static GTEXT_Markup_Status membuf_write(void * user, const char * bytes, size_t len) {
  membuf * b = user;
  if (len > SIZE_MAX - b->len - 1) {
    b->oom = true;
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->len + len + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + len + 1) {
      cap = cap > SIZE_MAX / 2 ? b->len + len + 1 : cap * 2;
    }
    char * grown = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!grown) {
      b->oom = true;
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, bytes, len);
  b->len += len;
  return GTEXT_MARKUP_OK;
}

static GTEXT_CSS_Status to_buffer(membuf * b, GTEXT_CSS_Status st, char ** out, size_t * out_length) {
  if (b->oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK && !b->data) {
    b->data = gtext_allocator_malloc(&b->alloc, 1);
    if (!b->data) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&b->alloc, b->data);
    return st;
  }
  b->data[b->len] = '\0';
  *out = b->data;
  *out_length = b->len;
  return GTEXT_CSS_OK;
}

static void membuf_init(membuf * b, const GTEXT_CSS_Serialize_Options * options) {
  memset(b, 0, sizeof *b);
  b->alloc = *(options && options->allocator ? options->allocator : gtext_allocator_default());
}

GTEXT_CSS_Status gtext_css_serialize_tree_buffer(const GTEXT_CSS_Tree * tree,
    const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length) {
  if (!out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  membuf b;
  membuf_init(&b, options);
  GTEXT_Markup_Sink sink = {membuf_write, &b};
  const GTEXT_CSS_Status st = gtext_css_serialize_tree(tree, options, &sink);
  return to_buffer(&b, st == GTEXT_CSS_E_WRITE ? GTEXT_CSS_E_OOM : st, out, out_length);
}

GTEXT_CSS_Status gtext_css_serialize_values_buffer(const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length) {
  if (!out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  membuf b;
  membuf_init(&b, options);
  GTEXT_Markup_Sink sink = {membuf_write, &b};
  const GTEXT_CSS_Status st = gtext_css_serialize_values(values, count, options, &sink);
  return to_buffer(&b, st == GTEXT_CSS_E_WRITE ? GTEXT_CSS_E_OOM : st, out, out_length);
}

void gtext_css_serialize_buffer_free(const GTEXT_CSS_Serialize_Options * options, char * buffer) {
  if (!buffer) {
    return;
  }
  GTEXT_Allocator a = *(options && options->allocator ? options->allocator : gtext_allocator_default());
  gtext_allocator_free(&a, buffer);
}

/* ---- the idioms on their own ------------------------------------------ */

GTEXT_CSS_Status gtext_css_serialize_identifier(const char * text, size_t length,
    const GTEXT_Markup_Sink * sink) {
  writer w;
  if ((!text && length > 0) || !writer_init(&w, NULL, sink)) {
    return GTEXT_CSS_E_INVALID;
  }
  put_ident(&w, text, length, ID_PLAIN);
  return w.status;
}

GTEXT_CSS_Status gtext_css_serialize_string(const char * text, size_t length,
    const GTEXT_Markup_Sink * sink) {
  writer w;
  if ((!text && length > 0) || !writer_init(&w, NULL, sink)) {
    return GTEXT_CSS_E_INVALID;
  }
  put_string(&w, text ? text : "", length);
  return w.status;
}
