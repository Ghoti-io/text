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
 * @file toml_lexer.c
 * @brief Scanning TOML's lexical elements: keys, strings, numbers, date-times.
 */

#include "../text_number_internal.h"
#include "toml_internal.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*--------------------------------------------------------------------------*
 * Byte buffer
 *--------------------------------------------------------------------------*/

bool toml_buf_reserve(
    const GTEXT_Allocator * alloc, toml_buf * buf, size_t extra) {
  if (buf->len + extra + 1 <= buf->capacity) return true;
  size_t want = buf->capacity ? buf->capacity : 32;
  while (want < buf->len + extra + 1) {
    if (want > SIZE_MAX / 2) return false;
    want *= 2;
  }
  char * grown = gtext_allocator_realloc(alloc, buf->data, want);
  if (!grown) return false;
  buf->data = grown;
  buf->capacity = want;
  return true;
}

bool toml_buf_append(const GTEXT_Allocator * alloc, toml_buf * buf,
    const char * bytes, size_t len) {
  if (!toml_buf_reserve(alloc, buf, len)) return false;
  if (len) memcpy(buf->data + buf->len, bytes, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
  return true;
}

bool toml_buf_append_byte(
    const GTEXT_Allocator * alloc, toml_buf * buf, char byte) {
  return toml_buf_append(alloc, buf, &byte, 1);
}

bool toml_buf_append_utf8(
    const GTEXT_Allocator * alloc, toml_buf * buf, uint32_t scalar) {
  char out[4];
  size_t n;
  if (scalar < 0x80) {
    out[0] = (char) scalar;
    n = 1;
  }
  else if (scalar < 0x800) {
    out[0] = (char) (0xC0 | (scalar >> 6));
    out[1] = (char) (0x80 | (scalar & 0x3F));
    n = 2;
  }
  else if (scalar < 0x10000) {
    out[0] = (char) (0xE0 | (scalar >> 12));
    out[1] = (char) (0x80 | ((scalar >> 6) & 0x3F));
    out[2] = (char) (0x80 | (scalar & 0x3F));
    n = 3;
  }
  else {
    out[0] = (char) (0xF0 | (scalar >> 18));
    out[1] = (char) (0x80 | ((scalar >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((scalar >> 6) & 0x3F));
    out[3] = (char) (0x80 | (scalar & 0x3F));
    n = 4;
  }
  return toml_buf_append(alloc, buf, out, n);
}

void toml_buf_free(const GTEXT_Allocator * alloc, toml_buf * buf) {
  gtext_allocator_free(alloc, buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->capacity = 0;
}

/*--------------------------------------------------------------------------*
 * UTF-8
 *--------------------------------------------------------------------------*/

bool toml_utf8_next(const char * buf, size_t len, size_t pos, uint32_t * scalar,
    size_t * width) {
  if (pos >= len) return false;
  unsigned char c = (unsigned char) buf[pos];
  uint32_t value;
  size_t need;
  if (c < 0x80) {
    value = c;
    need = 1;
  }
  else if ((c & 0xE0) == 0xC0) {
    value = c & 0x1Fu;
    need = 2;
  }
  else if ((c & 0xF0) == 0xE0) {
    value = c & 0x0Fu;
    need = 3;
  }
  else if ((c & 0xF8) == 0xF0) {
    value = c & 0x07u;
    need = 4;
  }
  else {
    /* A continuation byte where a lead byte belongs, or 0xF8..0xFF which no
     * UTF-8 sequence begins with. */
    return false;
  }
  if (pos + need > len) return false;
  for (size_t i = 1; i < need; ++i) {
    unsigned char cc = (unsigned char) buf[pos + i];
    if ((cc & 0xC0) != 0x80) return false;
    value = (value << 6) | (cc & 0x3Fu);
  }
  /* The three things a byte-counting loop does not see. An overlong encoding
   * spells a scalar in more bytes than it needs, which is how a NUL or a '/'
   * gets past a check that compares bytes; a surrogate is not a scalar value;
   * and nothing above U+10FFFF is a character at all. */
  static const uint32_t minimum[5] = {0, 0, 0x80, 0x800, 0x10000};
  if (value < minimum[need]) return false;
  if (value >= 0xD800 && value <= 0xDFFF) return false;
  if (value > 0x10FFFF) return false;
  if (scalar) *scalar = value;
  if (width) *width = need;
  return true;
}

bool toml_utf8_validate(const char * buf, size_t len, size_t * bad) {
  size_t pos = 0;
  while (pos < len) {
    size_t width = 0;
    if (!toml_utf8_next(buf, len, pos, NULL, &width)) {
      if (bad) *bad = pos;
      return false;
    }
    pos += width;
  }
  return true;
}

/*--------------------------------------------------------------------------*
 * Character classes
 *--------------------------------------------------------------------------*/

static bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

static bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

static bool is_bare_key_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c)
      || c == '_' || c == '-';
}

/**
 * Whether a byte is a control character TOML forbids in text.
 *
 * Tab is allowed everywhere text is; every other C0 control and U+007F are
 * not. Newline is handled by the caller, because whether it is allowed depends
 * on which string form is being read - which is why this does not decide it.
 */
static bool is_forbidden_control(unsigned char c) {
  if (c == '\t') return false;
  return c < 0x20 || c == 0x7F;
}

/*--------------------------------------------------------------------------*
 * Strings
 *--------------------------------------------------------------------------*/

/** Copy one character's bytes, validating UTF-8. */
static bool copy_char(toml_ctx * ctx, toml_buf * out) {
  unsigned char c = (unsigned char) ctx->buf[ctx->pos];
  if (c < 0x80) {
    if (!toml_buf_append_byte(ctx->alloc, out, (char) c)) {
      return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    }
    ctx->pos++;
    return true;
  }
  size_t width = 0;
  if (!toml_utf8_next(ctx->buf, ctx->len, ctx->pos, NULL, &width)) {
    return toml_fail(ctx, GTEXT_TOML_E_BAD_UNICODE,
        "not valid UTF-8; a TOML document must be");
  }
  if (!toml_buf_append(ctx->alloc, out, ctx->buf + ctx->pos, width)) {
    return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
  }
  ctx->pos += width;
  return true;
}

/** Read `count` hex digits as a scalar value and append it as UTF-8. */
static bool scan_escaped_scalar(toml_ctx * ctx, toml_buf * out, size_t count) {
  size_t start = ctx->pos;
  if (ctx->pos + count > ctx->len) {
    return toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_ESCAPE,
        "a unicode escape needs its full complement of hex digits");
  }
  uint32_t scalar = 0;
  for (size_t i = 0; i < count; ++i) {
    char c = ctx->buf[ctx->pos + i];
    if (!is_hex(c)) {
      return toml_fail_at(ctx, ctx->pos + i, GTEXT_TOML_E_BAD_ESCAPE,
          "a unicode escape takes hex digits only");
    }
    scalar = (scalar << 4) | (uint32_t) hex_value(c);
  }
  ctx->pos += count;
  if ((scalar >= 0xD800 && scalar <= 0xDFFF) || scalar > 0x10FFFF) {
    /* Not E_BAD_ESCAPE: the escape is well formed and names something that is
     * not a Unicode scalar value, which is the same mistake as ill-formed
     * UTF-8 one level up. */
    return toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_UNICODE,
        "an escape must name a Unicode scalar value");
  }
  if (!toml_buf_append_utf8(ctx->alloc, out, scalar)) {
    return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
  }
  return true;
}

/**
 * Handle one escape sequence, `ctx->pos` on the backslash.
 *
 * @param multiline Whether a line-ending backslash is meaningful here.
 * @return false on failure, with the error recorded.
 */
static bool scan_escape(toml_ctx * ctx, toml_buf * out, bool multiline) {
  size_t backslash = ctx->pos;
  ctx->pos++; /* past the backslash */
  if (ctx->pos >= ctx->len) {
    return toml_fail_at(ctx, backslash, GTEXT_TOML_E_BAD_ESCAPE,
        "the document ends inside an escape sequence");
  }
  char c = ctx->buf[ctx->pos];

  if (multiline) {
    /* "When the last non-whitespace character on a line is an unescaped \, it
     * will be trimmed along with all whitespace (including newlines) up to the
     * next non-whitespace character." The whitespace between the backslash and
     * the newline is part of what is trimmed, so this looks past it before
     * deciding. */
    size_t look = ctx->pos;
    while (look < ctx->len
        && (ctx->buf[look] == ' ' || ctx->buf[look] == '\t')) {
      ++look;
    }
    bool at_newline = look < ctx->len
        && (ctx->buf[look] == '\n'
            || (ctx->buf[look] == '\r' && look + 1 < ctx->len
                && ctx->buf[look + 1] == '\n'));
    if (at_newline) {
      while (look < ctx->len) {
        char w = ctx->buf[look];
        if (w == '\n') {
          ctx->line++;
          ctx->line_start = look + 1;
          ++look;
        }
        else if (w == ' ' || w == '\t' || w == '\r') {
          ++look;
        }
        else {
          break;
        }
      }
      ctx->pos = look;
      return true;
    }
  }

  ctx->pos++;
  switch (c) {
    case 'b': return toml_buf_append_byte(ctx->alloc, out, '\b')
        || toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    case 't': return toml_buf_append_byte(ctx->alloc, out, '\t')
        || toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    case 'n': return toml_buf_append_byte(ctx->alloc, out, '\n')
        || toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    case 'f': return toml_buf_append_byte(ctx->alloc, out, '\f')
        || toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    case 'r': return toml_buf_append_byte(ctx->alloc, out, '\r')
        || toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    case '"': return toml_buf_append_byte(ctx->alloc, out, '"')
        || toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    case '\\': return toml_buf_append_byte(ctx->alloc, out, '\\')
        || toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    case 'u': return scan_escaped_scalar(ctx, out, 4);
    case 'U': return scan_escaped_scalar(ctx, out, 8);
    default:
      /* \e and \xHH are 1.1.0 additions, and this is the first of the three
       * places the version option reaches. Refused by name under 1.0.0, so the
       * message says which version would take them rather than only that this
       * one will not. */
      if (c == 'e' || c == 'x') {
        if (ctx->version != GTEXT_TOML_VERSION_1_1_0) {
          return toml_fail_at(ctx, backslash, GTEXT_TOML_E_BAD_ESCAPE,
              "that escape is a TOML 1.1.0 addition; this parse is 1.0.0");
        }
        if (c == 'e') {
          return toml_buf_append_byte(ctx->alloc, out, '\x1B')
              || toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
        }
        /* `\xHH` names U+00HH, a scalar value and not a byte: 1.1.0 says "all
         * TOML strings are sequences of Unicode characters, _not_ byte
         * sequences", so \xf8 is two bytes of UTF-8 and not one of Latin-1.
         * Writing it as a byte is the mistake this shares a scanner with
         * \uXXXX to avoid. */
        return scan_escaped_scalar(ctx, out, 2);
      }
      return toml_fail_at(
          ctx, backslash, GTEXT_TOML_E_BAD_ESCAPE, "not an escape sequence");
  }
}

/** Consume a newline, which is LF or CRLF and never a lone CR. */
static bool consume_newline(toml_ctx * ctx, toml_buf * out, bool keep) {
  if (ctx->buf[ctx->pos] == '\r') {
    if (ctx->pos + 1 >= ctx->len || ctx->buf[ctx->pos + 1] != '\n') {
      return toml_fail(ctx, GTEXT_TOML_E_CONTROL,
          "a carriage return is only a newline when a line feed follows it");
    }
    ctx->pos++;
  }
  /* CRLF becomes LF in a string's value. TOML says a parser may normalize
   * newlines to what suits its platform; this normalizes to LF so that one
   * document read on two platforms gives one value, which is what a
   * differential against another implementation needs. */
  if (keep && !toml_buf_append_byte(ctx->alloc, out, '\n')) {
    return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
  }
  ctx->pos++;
  ctx->line++;
  ctx->line_start = ctx->pos;
  return true;
}

/**
 * Scan a multi-line string body, `ctx->pos` just past the opening delimiter.
 *
 * @param quote '"' for basic, '\'' for literal.
 */
static bool scan_multiline(toml_ctx * ctx, toml_buf * out, char quote) {
  /* "A newline immediately following the opening delimiter will be trimmed." */
  if (ctx->pos < ctx->len
      && (ctx->buf[ctx->pos] == '\n'
          || (ctx->buf[ctx->pos] == '\r' && ctx->pos + 1 < ctx->len
              && ctx->buf[ctx->pos + 1] == '\n'))) {
    if (!consume_newline(ctx, out, false)) return false;
  }
  for (;;) {
    if (ctx->pos >= ctx->len) {
      return toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
          "the document ends inside a multi-line string");
    }
    char c = ctx->buf[ctx->pos];
    if (c == quote) {
      size_t run = 0;
      while (ctx->pos + run < ctx->len && ctx->buf[ctx->pos + run] == quote) {
        ++run;
      }
      if (run < 3) {
        for (size_t i = 0; i < run; ++i) {
          if (!toml_buf_append_byte(ctx->alloc, out, quote)) {
            return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
          }
        }
        ctx->pos += run;
        continue;
      }
      /* The closing delimiter is three, and up to two more may be content:
       * `"""x"""""` is `x""`. A sixth is left in the stream, where the
       * statement parser refuses it - which is what the reference does, and
       * the reason this absorbs at most two rather than all of them. */
      size_t extra = run - 3;
      if (extra > 2) extra = 2;
      for (size_t i = 0; i < extra; ++i) {
        if (!toml_buf_append_byte(ctx->alloc, out, quote)) {
          return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
        }
      }
      ctx->pos += 3 + extra;
      return true;
    }
    if (c == '\\' && quote == '"') {
      if (!scan_escape(ctx, out, true)) return false;
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (!consume_newline(ctx, out, true)) return false;
      continue;
    }
    if (is_forbidden_control((unsigned char) c)) {
      return toml_fail(ctx, GTEXT_TOML_E_CONTROL,
          "a control character other than tab cannot appear in a string");
    }
    if (!copy_char(ctx, out)) return false;
  }
}

/** Scan a single-line string body, `ctx->pos` just past the opening quote. */
static bool scan_singleline(toml_ctx * ctx, toml_buf * out, char quote) {
  for (;;) {
    if (ctx->pos >= ctx->len) {
      return toml_fail(
          ctx, GTEXT_TOML_E_BAD_TOKEN, "the document ends inside a string");
    }
    char c = ctx->buf[ctx->pos];
    if (c == quote) {
      ctx->pos++;
      return true;
    }
    if (c == '\\' && quote == '"') {
      if (!scan_escape(ctx, out, false)) return false;
      continue;
    }
    if (c == '\n' || c == '\r') {
      return toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
          "a single-line string cannot contain a newline");
    }
    if (is_forbidden_control((unsigned char) c)) {
      return toml_fail(ctx, GTEXT_TOML_E_CONTROL,
          "a control character other than tab cannot appear in a string");
    }
    if (!copy_char(ctx, out)) return false;
  }
}

bool toml_scan_string(toml_ctx * ctx, toml_buf * out) {
  char quote = ctx->buf[ctx->pos];
  bool triple = ctx->pos + 2 < ctx->len && ctx->buf[ctx->pos + 1] == quote
      && ctx->buf[ctx->pos + 2] == quote;
  if (triple) {
    ctx->pos += 3;
    return scan_multiline(ctx, out, quote);
  }
  ctx->pos += 1;
  return scan_singleline(ctx, out, quote);
}

/*--------------------------------------------------------------------------*
 * Keys
 *--------------------------------------------------------------------------*/

bool toml_scan_key(toml_ctx * ctx, toml_buf * out) {
  if (ctx->pos >= ctx->len) {
    return toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN, "a key was expected");
  }
  char c = ctx->buf[ctx->pos];
  if (c == '"' || c == '\'') {
    /* A key is a basic or literal string, never a multi-line one: the
     * specification's `quoted-key` production names the single-line forms
     * only, and a key spanning lines could not be written on a key-value
     * line. Checked here rather than left to the string scanner, which would
     * accept it and leave the refusal to whatever came after. */
    bool triple = ctx->pos + 2 < ctx->len && ctx->buf[ctx->pos + 1] == c
        && ctx->buf[ctx->pos + 2] == c;
    if (triple) {
      return toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN,
          "a key cannot be a multi-line string");
    }
    return toml_scan_string(ctx, out);
  }
  if (!is_bare_key_char(c)) {
    return toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN, "a key was expected");
  }
  size_t start = ctx->pos;
  while (ctx->pos < ctx->len && is_bare_key_char(ctx->buf[ctx->pos])) {
    ctx->pos++;
  }
  if (!toml_buf_append(ctx->alloc, out, ctx->buf + start, ctx->pos - start)) {
    return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
  }
  return true;
}

/*--------------------------------------------------------------------------*
 * Numbers, booleans and date-times
 *--------------------------------------------------------------------------*/

/** Whether what follows looks like a date-time rather than a number. */
static bool looks_like_datetime(const toml_ctx * ctx) {
  size_t left = ctx->len - ctx->pos;
  const char * p = ctx->buf + ctx->pos;
  if (left >= 5 && is_digit(p[0]) && is_digit(p[1]) && is_digit(p[2])
      && is_digit(p[3]) && p[4] == '-') {
    return true;
  }
  if (left >= 3 && is_digit(p[0]) && is_digit(p[1]) && p[2] == ':') {
    return true;
  }
  return false;
}

/** Consume `count` digits, or fail. */
static bool take_digits(toml_ctx * ctx, size_t count) {
  if (ctx->pos + count > ctx->len) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!is_digit(ctx->buf[ctx->pos + i])) return false;
  }
  ctx->pos += count;
  return true;
}

/**
 * Consume a `HH:MM[:SS][.fraction]` time.
 *
 * @param secs_at Receives SIZE_MAX when seconds were written, and otherwise
 *   the offset just past the minutes - the point where 1.1.0's `:00` belongs.
 *   Whether an omission is allowed is not decided here: the extent is the same
 *   either way, and the caller has the position to complain about.
 */
static bool take_time(toml_ctx * ctx, size_t * secs_at) {
  *secs_at = SIZE_MAX;
  if (!take_digits(ctx, 2)) return false;
  if (ctx->pos >= ctx->len || ctx->buf[ctx->pos] != ':') return false;
  ctx->pos++;
  if (!take_digits(ctx, 2)) return false;
  if (ctx->pos < ctx->len && ctx->buf[ctx->pos] == ':') {
    ctx->pos++;
    if (!take_digits(ctx, 2)) return false;
    if (ctx->pos < ctx->len && ctx->buf[ctx->pos] == '.') {
      ctx->pos++;
      if (ctx->pos >= ctx->len || !is_digit(ctx->buf[ctx->pos])) return false;
      while (ctx->pos < ctx->len && is_digit(ctx->buf[ctx->pos])) ctx->pos++;
    }
    return true;
  }
  /* A fraction cannot follow minutes: 1.1.0's grammar puts it on the seconds,
   * so `07:32.5` is not a time with a fractional minute. Left in the stream for
   * the statement parser, which is where any other trailing byte is refused. */
  *secs_at = ctx->pos;
  return true;
}

/**
 * Scan a date-time and let `chron` decide whether it is one.
 *
 * This finds the extent and nothing more: the grammar, the leap-year rule, the
 * offset range and whether seconds may be omitted are all `chron`'s, which is
 * the whole reason for the dependency. Writing a second reading of them here
 * is how two modules of one library come to disagree about a date.
 */
static GTEXT_TOML_Value * scan_datetime(toml_ctx * ctx) {
  size_t start = ctx->pos;
  bool has_date = false;
  size_t secs_at = SIZE_MAX;

  if (ctx->len - ctx->pos >= 5 && is_digit(ctx->buf[ctx->pos + 4]) == false
      && ctx->buf[ctx->pos + 4] == '-') {
    if (!take_digits(ctx, 4)) {
      toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not a date");
      return NULL;
    }
    ctx->pos++; /* '-' */
    if (!take_digits(ctx, 2) || ctx->pos >= ctx->len
        || ctx->buf[ctx->pos] != '-') {
      toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not a date");
      return NULL;
    }
    ctx->pos++; /* '-' */
    if (!take_digits(ctx, 2)) {
      toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not a date");
      return NULL;
    }
    has_date = true;
  }

  if (has_date) {
    /* A date may be followed by a time, separated by T, t or a space. The
     * space is only a separator when a time really follows: `d = 1979-05-27 #
     * comment` is a local date and a comment, and a scanner that swallowed the
     * space would make it a syntax error. */
    if (ctx->pos < ctx->len) {
      char sep = ctx->buf[ctx->pos];
      bool sep_ok = sep == 'T' || sep == 't' || sep == ' ';
      if (sep_ok && ctx->pos + 3 < ctx->len && is_digit(ctx->buf[ctx->pos + 1])
          && is_digit(ctx->buf[ctx->pos + 2])
          && ctx->buf[ctx->pos + 3] == ':') {
        ctx->pos++;
        if (!take_time(ctx, &secs_at)) {
          toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not a time");
          return NULL;
        }
        if (ctx->pos < ctx->len) {
          char z = ctx->buf[ctx->pos];
          if (z == 'Z' || z == 'z') {
            ctx->pos++;
          }
          else if (z == '+' || z == '-') {
            ctx->pos++;
            if (!take_digits(ctx, 2) || ctx->pos >= ctx->len
                || ctx->buf[ctx->pos] != ':') {
              toml_fail_at(
                  ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not an offset");
              return NULL;
            }
            ctx->pos++;
            if (!take_digits(ctx, 2)) {
              toml_fail_at(
                  ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not an offset");
              return NULL;
            }
          }
        }
      }
    }
  }
  else {
    if (!take_time(ctx, &secs_at)) {
      toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not a time");
      return NULL;
    }
  }

  const char * text = ctx->buf + start;
  size_t text_len = ctx->pos - start;
  /* Bounded by the grammar rather than by a guess: a time with no seconds has
   * no fraction either, so the longest such date-time is
   * `YYYY-MM-DDTHH:MM+HH:MM`, 22 bytes, and 25 once `:00` is in. The check
   * below is still written out, because a bound argued from the grammar and a
   * bound the code enforces are two different things. */
  char patched[32];
  if (secs_at != SIZE_MAX) {
    /* The second of the three places the version option reaches. 1.1.0:
     * "seconds may be omitted, in which case `:00` will be assumed" - which is
     * a rewrite of the text, so that is what this does rather than a second
     * reading of the grammar. chron stays the only date-time parser here. */
    if (ctx->version != GTEXT_TOML_VERSION_1_1_0) {
      toml_fail_at(ctx, secs_at, GTEXT_TOML_E_BAD_TOKEN,
          "a TOML 1.0.0 time needs its seconds; 1.1.0 makes them optional");
      return NULL;
    }
    size_t head = secs_at - start;
    if (text_len + 3 > sizeof(patched)) {
      toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not a time");
      return NULL;
    }
    memcpy(patched, text, head);
    memcpy(patched + head, ":00", 3);
    memcpy(patched + head + 3, text + head, text_len - head);
    text = patched;
    text_len += 3;
  }

  GCHRON_TomlValue parsed;
  GCHRON_Result result =
      gchron_parse_toml(text, text_len, NULL, &parsed, NULL, NULL);
  if (result != GCHRON_OK) {
    toml_fail_at(ctx, start, GTEXT_TOML_E_DATETIME,
        "not a TOML date-time; chron refused it");
    return NULL;
  }
  GTEXT_TOML_Value * value = toml_value_new(ctx->alloc, GTEXT_TOML_DATETIME);
  if (!value) {
    toml_fail_at(ctx, start, GTEXT_TOML_E_OOM, "out of memory");
    return NULL;
  }
  value->as.datetime = parsed;
  return value;
}

/** Whether the bytes at `pos` are `word` followed by a value terminator. */
static bool word_at(const toml_ctx * ctx, size_t pos, const char * word) {
  size_t n = strlen(word);
  if (pos + n > ctx->len) return false;
  if (memcmp(ctx->buf + pos, word, n) != 0) return false;
  if (pos + n == ctx->len) return true;
  char after = ctx->buf[pos + n];
  /* A value ends at whitespace, a comment, a separator or a newline. `truex`
   * is not `true` followed by `x`: it is not a value at all, and saying so
   * here rather than accepting a prefix is what makes the `invalid/bool` cases fail
   * for the right reason. */
  return after == ' ' || after == '\t' || after == '\n' || after == '\r'
      || after == '#' || after == ',' || after == ']' || after == '}';
}

/**
 * Copy a number's digits with the underscores removed, checking their
 * placement.
 *
 * TOML allows underscores "between digits", which rules out a leading one, a
 * trailing one and two in a row. The check is here rather than in the scanner
 * because the scanner would have to know what counts as a digit in each of the
 * four bases, and this already does.
 */
static bool strip_underscores(toml_ctx * ctx, size_t start, size_t end,
    bool (*digit)(char), toml_buf * out) {
  bool prev_digit = false;
  for (size_t i = start; i < end; ++i) {
    char c = ctx->buf[i];
    if (c == '_') {
      bool next_digit = i + 1 < end && digit(ctx->buf[i + 1]);
      if (!prev_digit || !next_digit) {
        return toml_fail_at(ctx, i, GTEXT_TOML_E_BAD_TOKEN,
            "an underscore in a number must have a digit on each side");
      }
      prev_digit = false;
      continue;
    }
    if (!toml_buf_append_byte(ctx->alloc, out, c)) {
      return toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
    }
    prev_digit = digit(c);
  }
  return true;
}

static bool is_oct(char c) {
  return c >= '0' && c <= '7';
}

static bool is_bin(char c) {
  return c == '0' || c == '1';
}

/** Scan a prefixed integer: 0x, 0o or 0b. */
static GTEXT_TOML_Value * scan_radix(toml_ctx * ctx, int base,
    bool (*digit)(char), const char * what) {
  size_t start = ctx->pos;
  ctx->pos += 2;
  size_t digits_start = ctx->pos;
  while (ctx->pos < ctx->len
      && (digit(ctx->buf[ctx->pos]) || ctx->buf[ctx->pos] == '_')) {
    ctx->pos++;
  }
  if (ctx->pos == digits_start) {
    toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_TOKEN, what);
    return NULL;
  }
  toml_buf digits = {0};
  if (!strip_underscores(ctx, digits_start, ctx->pos, digit, &digits)) {
    toml_buf_free(ctx->alloc, &digits);
    return NULL;
  }
  errno = 0;
  char * end = NULL;
  unsigned long long raw = strtoull(digits.data ? digits.data : "", &end, base);
  bool overflow = errno == ERANGE || raw > (unsigned long long) INT64_MAX;
  toml_buf_free(ctx->alloc, &digits);
  if (overflow) {
    /* The specification requires 64-bit signed at minimum, and says an integer
     * outside that range is an error rather than something to saturate. A
     * prefixed integer has no sign, so INT64_MAX is the whole of the range. */
    toml_fail_at(ctx, start, GTEXT_TOML_E_RANGE,
        "an integer outside the range of a signed 64-bit value");
    return NULL;
  }
  GTEXT_TOML_Value * value = toml_value_new(ctx->alloc, GTEXT_TOML_INTEGER);
  if (!value) {
    toml_fail_at(ctx, start, GTEXT_TOML_E_OOM, "out of memory");
    return NULL;
  }
  value->as.integer = (int64_t) raw;
  return value;
}

/** Scan a decimal integer or a float. */
static GTEXT_TOML_Value * scan_decimal(toml_ctx * ctx) {
  size_t start = ctx->pos;
  if (ctx->pos < ctx->len
      && (ctx->buf[ctx->pos] == '+' || ctx->buf[ctx->pos] == '-')) {
    ctx->pos++;
  }
  size_t int_start = ctx->pos;
  while (ctx->pos < ctx->len
      && (is_digit(ctx->buf[ctx->pos]) || ctx->buf[ctx->pos] == '_')) {
    ctx->pos++;
  }
  if (ctx->pos == int_start) {
    toml_fail_at(ctx, start, GTEXT_TOML_E_BAD_TOKEN, "not a value");
    return NULL;
  }
  /* "Leading zeroes are not allowed." A single zero is a number; `01` is not,
   * and neither is `0_1`. The integer part is checked here and the fractional
   * part deliberately is not: `0.01` is a float and its fraction may have as
   * many leading zeroes as it likes. */
  if (ctx->buf[int_start] == '0' && ctx->pos - int_start > 1) {
    toml_fail_at(ctx, int_start, GTEXT_TOML_E_BAD_TOKEN,
        "a number cannot have a leading zero");
    return NULL;
  }

  bool is_float = false;
  if (ctx->pos < ctx->len && ctx->buf[ctx->pos] == '.') {
    is_float = true;
    ctx->pos++;
    size_t frac_start = ctx->pos;
    while (ctx->pos < ctx->len
        && (is_digit(ctx->buf[ctx->pos]) || ctx->buf[ctx->pos] == '_')) {
      ctx->pos++;
    }
    if (ctx->pos == frac_start) {
      toml_fail_at(ctx, ctx->pos, GTEXT_TOML_E_BAD_TOKEN,
          "a decimal point must be followed by a digit");
      return NULL;
    }
  }
  if (ctx->pos < ctx->len
      && (ctx->buf[ctx->pos] == 'e' || ctx->buf[ctx->pos] == 'E')) {
    is_float = true;
    ctx->pos++;
    if (ctx->pos < ctx->len
        && (ctx->buf[ctx->pos] == '+' || ctx->buf[ctx->pos] == '-')) {
      ctx->pos++;
    }
    size_t exp_start = ctx->pos;
    while (ctx->pos < ctx->len
        && (is_digit(ctx->buf[ctx->pos]) || ctx->buf[ctx->pos] == '_')) {
      ctx->pos++;
    }
    if (ctx->pos == exp_start) {
      toml_fail_at(ctx, ctx->pos, GTEXT_TOML_E_BAD_TOKEN,
          "an exponent must have digits");
      return NULL;
    }
  }

  toml_buf text = {0};
  if (!strip_underscores(ctx, start, ctx->pos, is_digit, &text)) {
    toml_buf_free(ctx->alloc, &text);
    return NULL;
  }
  const char * digits = text.data ? text.data : "";

  GTEXT_TOML_Value * value = NULL;
  if (is_float) {
    /* gtext_number_strtod() rather than strtod(): strtod reads the decimal
     * point LC_NUMERIC names, so in a comma locale it would stop at the point
     * and read 3.14 as 3. That is the defect notes/text/LOCALE-INDEPENDENT-
     * NUMBERS.md is about, and this module gets it right by reusing the fix
     * rather than by being careful. */
    char * end = NULL;
    double d = gtext_number_strtod(ctx->alloc, digits, &end);
    toml_buf_free(ctx->alloc, &text);
    value = toml_value_new(ctx->alloc, GTEXT_TOML_FLOAT);
    if (!value) {
      toml_fail_at(ctx, start, GTEXT_TOML_E_OOM, "out of memory");
      return NULL;
    }
    value->as.floating = d;
    return value;
  }

  errno = 0;
  char * end = NULL;
  long long raw = strtoll(digits, &end, 10);
  bool overflow = errno == ERANGE;
  toml_buf_free(ctx->alloc, &text);
  if (overflow) {
    toml_fail_at(ctx, start, GTEXT_TOML_E_RANGE,
        "an integer outside the range of a signed 64-bit value");
    return NULL;
  }
  value = toml_value_new(ctx->alloc, GTEXT_TOML_INTEGER);
  if (!value) {
    toml_fail_at(ctx, start, GTEXT_TOML_E_OOM, "out of memory");
    return NULL;
  }
  value->as.integer = (int64_t) raw;
  return value;
}

GTEXT_TOML_Value * toml_scan_atom(toml_ctx * ctx) {
  if (ctx->pos >= ctx->len) {
    toml_fail(ctx, GTEXT_TOML_E_BAD_TOKEN, "a value was expected");
    return NULL;
  }

  if (word_at(ctx, ctx->pos, "true") || word_at(ctx, ctx->pos, "false")) {
    bool truth = ctx->buf[ctx->pos] == 't';
    ctx->pos += truth ? 4 : 5;
    GTEXT_TOML_Value * value = toml_value_new(ctx->alloc, GTEXT_TOML_BOOLEAN);
    if (!value) {
      toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
      return NULL;
    }
    value->as.boolean = truth;
    return value;
  }

  if (looks_like_datetime(ctx)) return scan_datetime(ctx);

  /* inf and nan, each with an optional sign. They are floats, and they are the
   * only values that begin with letters and are not keywords. */
  size_t after_sign = ctx->pos;
  bool negative = false;
  if (ctx->buf[ctx->pos] == '+' || ctx->buf[ctx->pos] == '-') {
    negative = ctx->buf[ctx->pos] == '-';
    after_sign = ctx->pos + 1;
  }
  if (word_at(ctx, after_sign, "inf") || word_at(ctx, after_sign, "nan")) {
    bool infinite = ctx->buf[after_sign] == 'i';
    ctx->pos = after_sign + 3;
    GTEXT_TOML_Value * value = toml_value_new(ctx->alloc, GTEXT_TOML_FLOAT);
    if (!value) {
      toml_fail(ctx, GTEXT_TOML_E_OOM, "out of memory");
      return NULL;
    }
    if (infinite) {
      value->as.floating = negative ? -INFINITY : INFINITY;
    }
    else {
      /* TOML has -nan and +nan and says nothing about which NaN either is, so
       * the sign is carried rather than interpreted. */
      value->as.floating = negative ? -NAN : NAN;
    }
    return value;
  }

  if (ctx->len - ctx->pos >= 2 && ctx->buf[ctx->pos] == '0') {
    char kind = ctx->buf[ctx->pos + 1];
    if (kind == 'x') return scan_radix(ctx, 16, is_hex, "not a hex integer");
    if (kind == 'o') return scan_radix(ctx, 8, is_oct, "not an octal integer");
    if (kind == 'b') return scan_radix(ctx, 2, is_bin, "not a binary integer");
  }

  return scan_decimal(ctx);
}
