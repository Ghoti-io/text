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
 * The CSS tokenizer: CSS Syntax section 4, over the input stream of 3.3.
 *
 * The input is UTF-8, decoded here a code point at a time with the
 * preprocessing of "filter code points" applied as it is read: CR, CR LF and
 * FF are one LF, and U+0000 and a surrogate (reachable only in WTF-8 input)
 * are U+FFFD. Ill-formed UTF-8 is U+FFFD for each maximal subpart, as the
 * Encoding Standard's UTF-8 decoder does, with a parse error of this
 * library's own (`invalid-utf-8`).
 *
 * The algorithms are the section's, step by step, written to look ahead
 * before consuming instead of reconsuming: "the next input code point" is
 * peek(0), and nothing is ever pushed back.
 *
 * While the input's own tokens are produced, the brackets are matched as the
 * parser's component-value algorithms will match them (an opening token is
 * closed only by its mirror; any other closing token is a token), which
 * gives each opening token the index of its closing one, and the nesting
 * depth, which is held to `max_depth` here, once, for every entry point.
 * Token structure does not depend on where a rule begins or ends, so the
 * parser can rely on the matching.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "css_internal.h"

/// "The EOF code point".
#define CP_EOF (-1)

/// The tokenizer's cursor over a range of the input.
typedef struct {
  css_ctx * ctx; ///< The parse context.
  size_t pos;    ///< Next byte.
  size_t end;    ///< One past the last byte.
  size_t line;   ///< Of `pos`.
  size_t col;    ///< Of `pos`.
  bool urange;   ///< Unicode ranges allowed.
  bool main;     ///< The input's own tokens (limits, matching, errors).
} tz;

/* ---- the input stream ------------------------------------------------- */

/* The code point at byte p (p < t->end), preprocessed; *n bytes; *bad for an
   ill-formed sequence; *wide for one outside the BMP (two UTF-16 units). */
static int32_t decode_at(const tz * t, size_t p, size_t * n, bool * bad, bool * wide) {
  const unsigned char * s = t->ctx->in;
  const size_t len = t->end;
  const uint8_t b = s[p];
  *bad = false;
  *wide = false;
  if (b < 0x80) {
    *n = 1;
    if (b == '\r') {
      if (p + 1 < len && s[p + 1] == '\n') {
        *n = 2;
      }
      return '\n';
    }
    if (b == 0x0C) {
      return '\n';
    }
    if (b == 0) {
      return 0xFFFD;
    }
    return b;
  }
  uint8_t lower = 0x80;
  uint8_t upper = 0xBF;
  size_t need;
  uint32_t value;
  if (b >= 0xC2 && b <= 0xDF) {
    need = 1;
    value = b & 0x1Fu;
  }
  else if (b >= 0xE0 && b <= 0xEF) {
    need = 2;
    value = b & 0x0Fu;
    if (b == 0xE0) {
      lower = 0xA0;
    }
    else if (b == 0xED && !t->ctx->wtf8) {
      upper = 0x9F;
    }
  }
  else if (b >= 0xF0 && b <= 0xF4) {
    need = 3;
    value = b & 0x07u;
    if (b == 0xF0) {
      lower = 0x90;
    }
    else if (b == 0xF4) {
      upper = 0x8F;
    }
  }
  else {
    *n = 1;
    *bad = true;
    return 0xFFFD;
  }
  for (size_t i = 1; i <= need; i++) {
    if (p + i >= len) {
      *n = i;
      *bad = true;
      return 0xFFFD;
    }
    const uint8_t c = s[p + i];
    if (c < lower || c > upper) {
      *n = i;
      *bad = true;
      return 0xFFFD;
    }
    lower = 0x80;
    upper = 0xBF;
    value = (value << 6) | (c & 0x3Fu);
  }
  *n = need + 1;
  if (value >= 0xD800 && value <= 0xDFFF) {
    return 0xFFFD; /* WTF-8: "filter code points" replaces a surrogate */
  }
  *wide = value > 0xFFFF;
  return (int32_t)value;
}

/* The k-th code point after the cursor (0 is "the next input code point"). */
static int32_t peek(const tz * t, int k) {
  size_t p = t->pos;
  t->ctx->work++;
  for (;;) {
    if (p >= t->end) {
      return CP_EOF;
    }
    size_t n;
    bool bad, wide;
    const int32_t cp = decode_at(t, p, &n, &bad, &wide);
    if (k == 0) {
      return cp;
    }
    k--;
    p += n;
  }
}

static int32_t consume(tz * t) {
  t->ctx->work++;
  if (t->pos >= t->end) {
    return CP_EOF;
  }
  size_t n;
  bool bad, wide;
  const int32_t cp = decode_at(t, t->pos, &n, &bad, &wide);
  if (bad && t->main && t->pos >= t->ctx->utf8_err_high) {
    css_error(t->ctx, GTEXT_CSS_ERROR_INVALID_UTF_8, t->pos, t->line, t->col);
    t->ctx->utf8_err_high = t->pos + n;
  }
  t->pos += n;
  if (cp == '\n') {
    t->line++;
    t->col = 1;
  }
  else {
    t->col += wide ? 2 : 1;
  }
  return cp;
}

static void error_here(tz * t, GTEXT_CSS_Error_Code code) {
  if (t->main) {
    css_error(t->ctx, code, t->pos, t->line, t->col);
  }
}

/* ---- definitions (4.2) ------------------------------------------------ */

static bool is_digit(int32_t c) {
  return c >= '0' && c <= '9';
}

static bool is_hex(int32_t c) {
  return is_digit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static int hex_value(int32_t c) {
  if (is_digit(c)) {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

static bool is_letter(int32_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_ident_start(int32_t c) {
  return is_letter(c) || c == '_' || (c >= 0x80 && css_is_non_ascii_ident((uint32_t)c));
}

static bool is_ident_cp(int32_t c) {
  return is_ident_start(c) || is_digit(c) || c == '-';
}

static bool is_non_printable(int32_t c) {
  return (c >= 0 && c <= 0x08) || c == 0x0B || (c >= 0x0E && c <= 0x1F) || c == 0x7F;
}

static bool is_ws(int32_t c) {
  return c == '\n' || c == '\t' || c == ' ';
}

/* 4.3.8 */
static bool valid_escape(int32_t a, int32_t b) {
  return a == '\\' && b != '\n';
}

/* 4.3.9 */
static bool starts_ident(int32_t a, int32_t b, int32_t c) {
  if (a == '-') {
    return is_ident_start(b) || b == '-' || valid_escape(b, c);
  }
  if (is_ident_start(a)) {
    return true;
  }
  if (a == '\\') {
    return valid_escape(a, b);
  }
  return false;
}

/* 4.3.10 */
static bool starts_number(int32_t a, int32_t b, int32_t c) {
  if (a == '+' || a == '-') {
    if (is_digit(b)) {
      return true;
    }
    return b == '.' && is_digit(c);
  }
  if (a == '.') {
    return is_digit(b);
  }
  return is_digit(a);
}

/* 4.3.11 */
static bool starts_urange(int32_t a, int32_t b, int32_t c) {
  return (a == 'U' || a == 'u') && b == '+' && (c == '?' || is_hex(c));
}

/* ---- consuming (4.3.7, 4.3.12, 4.3.13) -------------------------------- */

/* 4.3.7: the U+005C has been consumed and the escape is valid. */
static uint32_t consume_escaped(tz * t) {
  if (peek(t, 0) == CP_EOF) {
    error_here(t, GTEXT_CSS_ERROR_EOF_IN_ESCAPE);
    return 0xFFFD;
  }
  const int32_t c = consume(t);
  if (is_hex(c)) {
    uint32_t v = (uint32_t)hex_value(c);
    for (int i = 0; i < 5 && is_hex(peek(t, 0)); i++) {
      v = v * 16 + (uint32_t)hex_value(consume(t));
    }
    if (is_ws(peek(t, 0))) {
      consume(t);
    }
    if (v == 0 || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
      return 0xFFFD;
    }
    return v;
  }
  return (uint32_t)c;
}

/* 4.3.12, into `buf`. */
static bool consume_ident_seq(tz * t, css_buf * buf) {
  for (;;) {
    const int32_t c = peek(t, 0);
    if (is_ident_cp(c)) {
      consume(t);
      if (!css_buf_put_cp(t->ctx, buf, (uint32_t)c)) {
        return false;
      }
    }
    else if (valid_escape(c, peek(t, 1))) {
      consume(t);
      if (!css_buf_put_cp(t->ctx, buf, consume_escaped(t))) {
        return false;
      }
    }
    else {
      return true;
    }
  }
}

static bool put_ascii(tz * t, css_buf * buf, int32_t c) {
  const char ch = (char)c;
  return css_buf_put(t->ctx, buf, &ch, 1);
}

/* 4.3.13: the number's spelling goes to ctx->nbuf; the value, type and sign
   to the token. */
static bool consume_number(tz * t, css_tok * tok) {
  css_buf * nb = &t->ctx->nbuf;
  nb->len = 0;
  bool integer = true;
  int32_t c = peek(t, 0);
  if (c == '+' || c == '-') {
    tok->flags |= c == '+' ? GTEXT_CSS_FLAG_SIGN_PLUS : GTEXT_CSS_FLAG_SIGN_MINUS;
    if (!put_ascii(t, nb, consume(t))) {
      return false;
    }
  }
  while (is_digit(peek(t, 0))) {
    if (!put_ascii(t, nb, consume(t))) {
      return false;
    }
  }
  if (peek(t, 0) == '.' && is_digit(peek(t, 1))) {
    if (!put_ascii(t, nb, consume(t))) {
      return false;
    }
    while (is_digit(peek(t, 0))) {
      if (!put_ascii(t, nb, consume(t))) {
        return false;
      }
    }
    integer = false;
  }
  c = peek(t, 0);
  if (c == 'e' || c == 'E') {
    const int32_t c1 = peek(t, 1);
    if (is_digit(c1) || ((c1 == '+' || c1 == '-') && is_digit(peek(t, 2)))) {
      if (!put_ascii(t, nb, consume(t))) {
        return false;
      }
      if (c1 == '+' || c1 == '-') {
        if (!put_ascii(t, nb, consume(t))) {
          return false;
        }
      }
      while (is_digit(peek(t, 0))) {
        if (!put_ascii(t, nb, consume(t))) {
          return false;
        }
      }
      integer = false;
    }
  }
  if (integer) {
    tok->flags |= GTEXT_CSS_FLAG_INTEGER;
  }
  /* nb is NUL-terminated by css_buf_put. */
  if (!css_number_value(t->ctx, nb->data, nb->len, &tok->number)) {
    return false;
  }
  char * repr = css_arena_strdup(&t->ctx->arena, nb->data, nb->len);
  if (!repr) {
    css_fail(t->ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  tok->repr.data = repr;
  tok->repr.length = nb->len;
  return true;
}

static bool set_value(tz * t, css_tok * tok, const css_buf * buf) {
  char * s = css_arena_strdup(&t->ctx->arena, buf->len ? buf->data : "", buf->len);
  if (!s) {
    css_fail(t->ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  tok->value.data = s;
  tok->value.length = buf->len;
  return true;
}

/* 4.3.3 */
static bool consume_numeric(tz * t, css_tok * tok) {
  if (!consume_number(t, tok)) {
    return false;
  }
  if (starts_ident(peek(t, 0), peek(t, 1), peek(t, 2))) {
    tok->type = GTEXT_CSS_TOKEN_DIMENSION;
    css_buf * ub = &t->ctx->ubuf;
    ub->len = 0;
    if (!consume_ident_seq(t, ub)) {
      return false;
    }
    char * s = css_arena_strdup(&t->ctx->arena, ub->len ? ub->data : "", ub->len);
    if (!s) {
      css_fail(t->ctx, GTEXT_CSS_E_OOM);
      return false;
    }
    tok->unit.data = s;
    tok->unit.length = ub->len;
    return true;
  }
  if (peek(t, 0) == '%') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_PERCENTAGE;
    tok->flags &= (uint8_t)~GTEXT_CSS_FLAG_INTEGER; /* a percentage has no type flag */
    return true;
  }
  tok->type = GTEXT_CSS_TOKEN_NUMBER;
  return true;
}

/* 4.3.15 */
static void consume_bad_url_remnants(tz * t) {
  for (;;) {
    const int32_t c = peek(t, 0);
    if (c == CP_EOF) {
      return;
    }
    consume(t);
    if (c == ')') {
      return;
    }
    if (valid_escape(c, peek(t, 0))) {
      (void)consume_escaped(t);
    }
  }
}

/* 4.3.6: "url(" has been consumed. */
static bool consume_url(tz * t, css_tok * tok) {
  css_buf * sb = &t->ctx->sbuf;
  sb->len = 0;
  tok->type = GTEXT_CSS_TOKEN_URL;
  while (is_ws(peek(t, 0))) {
    consume(t);
  }
  for (;;) {
    const int32_t c = peek(t, 0);
    if (c == ')') {
      consume(t);
      return set_value(t, tok, sb);
    }
    if (c == CP_EOF) {
      error_here(t, GTEXT_CSS_ERROR_EOF_IN_URL);
      tok->flags |= GTEXT_CSS_FLAG_UNCLOSED;
      return set_value(t, tok, sb);
    }
    if (is_ws(c)) {
      while (is_ws(peek(t, 0))) {
        consume(t);
      }
      const int32_t d = peek(t, 0);
      if (d == ')') {
        consume(t);
        return set_value(t, tok, sb);
      }
      if (d == CP_EOF) {
        error_here(t, GTEXT_CSS_ERROR_EOF_IN_URL);
        tok->flags |= GTEXT_CSS_FLAG_UNCLOSED;
        return set_value(t, tok, sb);
      }
      error_here(t, GTEXT_CSS_ERROR_WHITESPACE_IN_URL);
      consume_bad_url_remnants(t);
      tok->type = GTEXT_CSS_TOKEN_BAD_URL;
      return true;
    }
    if (c == '"' || c == '\'' || c == '(' || is_non_printable(c)) {
      error_here(t, GTEXT_CSS_ERROR_INVALID_CHARACTER_IN_URL);
      consume(t);
      consume_bad_url_remnants(t);
      tok->type = GTEXT_CSS_TOKEN_BAD_URL;
      return true;
    }
    if (c == '\\') {
      if (valid_escape(c, peek(t, 1))) {
        consume(t);
        if (!css_buf_put_cp(t->ctx, sb, consume_escaped(t))) {
          return false;
        }
        continue;
      }
      error_here(t, GTEXT_CSS_ERROR_INVALID_ESCAPE_IN_URL);
      consume(t);
      consume_bad_url_remnants(t);
      tok->type = GTEXT_CSS_TOKEN_BAD_URL;
      return true;
    }
    consume(t);
    if (!css_buf_put_cp(t->ctx, sb, (uint32_t)c)) {
      return false;
    }
  }
}

/* 4.3.4 */
static bool consume_ident_like(tz * t, css_tok * tok) {
  css_buf * sb = &t->ctx->sbuf;
  sb->len = 0;
  if (!consume_ident_seq(t, sb)) {
    return false;
  }
  GTEXT_CSS_String s = {sb->data, sb->len};
  if (peek(t, 0) == '(' && css_ascii_ieq(&s, "url")) {
    consume(t);
    while (is_ws(peek(t, 0)) && is_ws(peek(t, 1))) {
      consume(t);
    }
    const int32_t a = peek(t, 0);
    const int32_t b = peek(t, 1);
    if (a == '"' || a == '\'' || (is_ws(a) && (b == '"' || b == '\''))) {
      tok->type = GTEXT_CSS_TOKEN_FUNCTION;
      return set_value(t, tok, sb);
    }
    return consume_url(t, tok);
  }
  if (peek(t, 0) == '(') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_FUNCTION;
    return set_value(t, tok, sb);
  }
  tok->type = GTEXT_CSS_TOKEN_IDENT;
  return set_value(t, tok, sb);
}

/* 4.3.5: the opening quote is the next code point. */
static bool consume_string(tz * t, css_tok * tok) {
  css_buf * sb = &t->ctx->sbuf;
  sb->len = 0;
  const int32_t ending = consume(t);
  tok->type = GTEXT_CSS_TOKEN_STRING;
  for (;;) {
    const int32_t c = peek(t, 0);
    if (c == ending) {
      consume(t);
      return set_value(t, tok, sb);
    }
    if (c == CP_EOF) {
      error_here(t, GTEXT_CSS_ERROR_EOF_IN_STRING);
      tok->flags |= GTEXT_CSS_FLAG_UNCLOSED;
      return set_value(t, tok, sb);
    }
    if (c == '\n') {
      error_here(t, GTEXT_CSS_ERROR_NEWLINE_IN_STRING);
      tok->type = GTEXT_CSS_TOKEN_BAD_STRING;
      return true;
    }
    consume(t);
    if (c == '\\') {
      const int32_t n = peek(t, 0);
      if (n == CP_EOF) {
        continue;
      }
      if (n == '\n') {
        consume(t);
        continue;
      }
      if (!css_buf_put_cp(t->ctx, sb, consume_escaped(t))) {
        return false;
      }
      continue;
    }
    if (!css_buf_put_cp(t->ctx, sb, (uint32_t)c)) {
      return false;
    }
  }
}

/* 4.3.14 */
static void consume_urange(tz * t, css_tok * tok) {
  consume(t); /* u */
  consume(t); /* + */
  uint32_t lo = 0, hi = 0;
  int digits = 0;
  while (digits < 6 && is_hex(peek(t, 0))) {
    const int v = hex_value(consume(t));
    lo = lo * 16 + (uint32_t)v;
    hi = hi * 16 + (uint32_t)v;
    digits++;
  }
  bool question = false;
  while (digits < 6 && peek(t, 0) == '?') {
    consume(t);
    lo = lo * 16;
    hi = hi * 16 + 15;
    digits++;
    question = true;
  }
  tok->type = GTEXT_CSS_TOKEN_UNICODE_RANGE;
  tok->delim = lo;
  if (question) {
    tok->range_end = hi;
    return;
  }
  if (peek(t, 0) == '-' && is_hex(peek(t, 1))) {
    consume(t);
    uint32_t end = 0;
    for (int i = 0; i < 6 && is_hex(peek(t, 0)); i++) {
      end = end * 16 + (uint32_t)hex_value(consume(t));
    }
    tok->range_end = end;
    return;
  }
  tok->range_end = lo;
}

static bool set_delim(tz * t, css_tok * tok, int32_t c) {
  tok->type = GTEXT_CSS_TOKEN_DELIM;
  tok->delim = (uint32_t)c;
  css_buf * sb = &t->ctx->sbuf;
  sb->len = 0;
  if (!css_buf_put_cp(t->ctx, sb, (uint32_t)c)) {
    return false;
  }
  return set_value(t, tok, sb);
}

/* 4.3.2 */
static void consume_comments(tz * t) {
  while (peek(t, 0) == '/' && peek(t, 1) == '*') {
    consume(t);
    consume(t);
    for (;;) {
      const int32_t c = peek(t, 0);
      if (c == CP_EOF) {
        error_here(t, GTEXT_CSS_ERROR_EOF_IN_COMMENT);
        return;
      }
      consume(t);
      if (c == '*' && peek(t, 0) == '/') {
        consume(t);
        break;
      }
    }
  }
}

/* 4.3.1: one token, or EOF. Returns false when the parse failed. */
static bool consume_token(tz * t, css_tok * tok) {
  consume_comments(t);
  memset(tok, 0, sizeof *tok);
  tok->start = t->pos;
  tok->line = t->line;
  tok->column = t->col;
  const int32_t c = peek(t, 0);
  bool ok = true;
  if (c == CP_EOF) {
    tok->type = GTEXT_CSS_TOKEN_EOF;
  }
  else if (is_ws(c)) {
    while (is_ws(peek(t, 0))) {
      consume(t);
    }
    tok->type = GTEXT_CSS_TOKEN_WHITESPACE;
  }
  else if (c == '"' || c == '\'') {
    ok = consume_string(t, tok);
  }
  else if (c == '#') {
    const int32_t a = peek(t, 1);
    const int32_t b = peek(t, 2);
    if (is_ident_cp(a) || valid_escape(a, b)) {
      consume(t);
      tok->type = GTEXT_CSS_TOKEN_HASH;
      if (starts_ident(a, b, peek(t, 2))) {
        tok->flags |= GTEXT_CSS_FLAG_HASH_ID;
      }
      css_buf * sb = &t->ctx->sbuf;
      sb->len = 0;
      ok = consume_ident_seq(t, sb) && set_value(t, tok, sb);
    }
    else {
      ok = set_delim(t, tok, consume(t));
    }
  }
  else if (c == '(') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_OPEN_PAREN;
  }
  else if (c == ')') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_CLOSE_PAREN;
  }
  else if (c == '+' || c == '.') {
    if (starts_number(c, peek(t, 1), peek(t, 2))) {
      ok = consume_numeric(t, tok);
    }
    else {
      ok = set_delim(t, tok, consume(t));
    }
  }
  else if (c == ',') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_COMMA;
  }
  else if (c == '-') {
    const int32_t a = peek(t, 1);
    const int32_t b = peek(t, 2);
    if (starts_number(c, a, b)) {
      ok = consume_numeric(t, tok);
    }
    else if (a == '-' && b == '>') {
      consume(t);
      consume(t);
      consume(t);
      tok->type = GTEXT_CSS_TOKEN_CDC;
    }
    else if (starts_ident(c, a, b)) {
      ok = consume_ident_like(t, tok);
    }
    else {
      ok = set_delim(t, tok, consume(t));
    }
  }
  else if (c == ':') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_COLON;
  }
  else if (c == ';') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_SEMICOLON;
  }
  else if (c == '<') {
    if (peek(t, 1) == '!' && peek(t, 2) == '-' && peek(t, 3) == '-') {
      consume(t);
      consume(t);
      consume(t);
      consume(t);
      tok->type = GTEXT_CSS_TOKEN_CDO;
    }
    else {
      ok = set_delim(t, tok, consume(t));
    }
  }
  else if (c == '@') {
    if (starts_ident(peek(t, 1), peek(t, 2), peek(t, 3))) {
      consume(t);
      tok->type = GTEXT_CSS_TOKEN_AT_KEYWORD;
      css_buf * sb = &t->ctx->sbuf;
      sb->len = 0;
      ok = consume_ident_seq(t, sb) && set_value(t, tok, sb);
    }
    else {
      ok = set_delim(t, tok, consume(t));
    }
  }
  else if (c == '[') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_OPEN_SQUARE;
  }
  else if (c == '\\') {
    if (valid_escape(c, peek(t, 1))) {
      ok = consume_ident_like(t, tok);
    }
    else {
      error_here(t, GTEXT_CSS_ERROR_INVALID_ESCAPE);
      ok = set_delim(t, tok, consume(t));
    }
  }
  else if (c == ']') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_CLOSE_SQUARE;
  }
  else if (c == '{') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_OPEN_CURLY;
  }
  else if (c == '}') {
    consume(t);
    tok->type = GTEXT_CSS_TOKEN_CLOSE_CURLY;
  }
  else if (is_digit(c)) {
    ok = consume_numeric(t, tok);
  }
  else if ((c == 'U' || c == 'u') && t->urange && starts_urange(c, peek(t, 1), peek(t, 2))) {
    consume_urange(t, tok);
  }
  else if (is_ident_start(c)) {
    ok = consume_ident_like(t, tok);
  }
  else {
    ok = set_delim(t, tok, consume(t));
  }
  tok->end = t->pos;
  return ok && t->ctx->status == GTEXT_CSS_OK;
}

static bool tokvec_push(css_ctx * ctx, css_tokvec * vec, const css_tok * tok) {
  if (vec->len == vec->cap) {
    const size_t cap = vec->cap ? vec->cap * 2 : 64;
    if (cap > SIZE_MAX / sizeof(css_tok)) {
      css_fail(ctx, GTEXT_CSS_E_OOM);
      return false;
    }
    css_tok * grown = gtext_allocator_realloc(&ctx->alloc, vec->data, cap * sizeof(css_tok));
    if (!grown) {
      css_fail(ctx, GTEXT_CSS_E_OOM);
      return false;
    }
    vec->data = grown;
    vec->cap = cap;
  }
  vec->data[vec->len++] = *tok;
  return true;
}

/** Free a token vector's array (css_internal.h). */
void css_tokvec_free(css_ctx * ctx, css_tokvec * vec) {
  gtext_allocator_free(&ctx->alloc, vec->data);
  vec->data = NULL;
  vec->len = vec->cap = 0;
}

static GTEXT_CSS_Token_Type closer_of(uint8_t type) {
  switch (type) {
  case GTEXT_CSS_TOKEN_FUNCTION:
  case GTEXT_CSS_TOKEN_OPEN_PAREN: return GTEXT_CSS_TOKEN_CLOSE_PAREN;
  case GTEXT_CSS_TOKEN_OPEN_SQUARE: return GTEXT_CSS_TOKEN_CLOSE_SQUARE;
  case GTEXT_CSS_TOKEN_OPEN_CURLY: return GTEXT_CSS_TOKEN_CLOSE_CURLY;
  default: return GTEXT_CSS_TOKEN_EOF;
  }
}

/** Tokenize a range of the input (css_internal.h). */
bool css_tokenize(css_ctx * ctx, size_t begin, size_t end, size_t line,
    size_t column, bool unicode_ranges, bool main, css_tokvec * out,
    css_tok * eof) {
  tz t;
  t.ctx = ctx;
  t.pos = begin;
  t.end = end;
  t.line = line;
  t.col = column;
  t.urange = unicode_ranges;
  t.main = main;
  const bool was_on = ctx->errors_on;
  if (!main) {
    ctx->errors_on = false;
  }
  css_sizevec match = {NULL, 0, 0};
  css_sizevec open = {NULL, 0, 0};
  bool ok = true;
  for (;;) {
    css_tok tok;
    if (!consume_token(&t, &tok)) {
      ok = false;
      break;
    }
    if (tok.type == GTEXT_CSS_TOKEN_EOF) {
      *eof = tok;
      break;
    }
    if (main) {
      if (out->len >= ctx->max_tokens) {
        css_fail_at(ctx, GTEXT_CSS_E_LIMIT_TOKENS, tok.start, tok.line, tok.column);
        ok = false;
        break;
      }
      if (!css_sizevec_push(ctx, &match, SIZE_MAX)) {
        ok = false;
        break;
      }
      const GTEXT_CSS_Token_Type closer = closer_of(tok.type);
      if (closer != GTEXT_CSS_TOKEN_EOF) {
        if (open.len >= ctx->max_depth) {
          css_fail_at(ctx, GTEXT_CSS_E_LIMIT_DEPTH, tok.start, tok.line, tok.column);
          ok = false;
          break;
        }
        if (!css_sizevec_push(ctx, &open, out->len)) {
          ok = false;
          break;
        }
      }
      else if (open.len > 0
          && closer_of(out->data[open.data[open.len - 1]].type) == (GTEXT_CSS_Token_Type)tok.type) {
        match.data[open.data[open.len - 1]] = out->len;
        open.len--;
      }
    }
    if (!tokvec_push(ctx, out, &tok)) {
      ok = false;
      break;
    }
  }
  ctx->errors_on = was_on;
  if (main) {
    ctx->end_line = t.line;
    ctx->end_column = t.col;
    if (ok) {
      for (size_t i = 0; i < open.len; i++) {
        match.data[open.data[i]] = out->len;
      }
      gtext_allocator_free(&ctx->alloc, ctx->match);
      ctx->match = match.data;
      match.data = NULL;
    }
  }
  gtext_allocator_free(&ctx->alloc, match.data);
  gtext_allocator_free(&ctx->alloc, open.data);
  return ok && ctx->status == GTEXT_CSS_OK;
}

/** Where the byte `offset` is (css_internal.h): the tokenizer's own counting. */
void css_position_of(css_ctx * ctx, size_t offset, size_t * line, size_t * column) {
  tz t;
  memset(&t, 0, sizeof t);
  t.ctx = ctx;
  t.end = ctx->in_len;
  t.line = 1;
  t.col = 1;
  while (t.pos < offset && t.pos < t.end) {
    size_t n;
    bool bad, wide;
    const int32_t cp = decode_at(&t, t.pos, &n, &bad, &wide);
    t.pos += n;
    if (cp == '\n') {
      t.line++;
      t.col = 1;
    }
    else {
      t.col += wide ? 2 : 1;
    }
  }
  *line = t.line;
  *column = t.col;
}

/* ---- original text ---------------------------------------------------- */

/** A range of the input with preprocessing applied (css_internal.h). */
bool css_filtered_text(css_ctx * ctx, size_t begin, size_t end,
    GTEXT_CSS_String * out) {
  tz t;
  memset(&t, 0, sizeof t);
  t.ctx = ctx;
  t.pos = begin;
  t.end = end;
  css_buf * sb = &ctx->sbuf;
  sb->len = 0;
  while (t.pos < t.end) {
    size_t n;
    bool bad, wide;
    const int32_t cp = decode_at(&t, t.pos, &n, &bad, &wide);
    t.pos += n;
    if (!css_buf_put_cp(ctx, sb, (uint32_t)cp)) {
      return false;
    }
  }
  char * s = css_arena_strdup(&ctx->arena, sb->len ? sb->data : "", sb->len);
  if (!s) {
    css_fail(ctx, GTEXT_CSS_E_OOM);
    return false;
  }
  out->data = s;
  out->length = sb->len;
  return true;
}
