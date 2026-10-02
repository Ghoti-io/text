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
 * Locale-independent number conversion.
 *
 * See text_number_internal.h for why this is shared rather than per-format,
 * and for the three defects that put it here.
 *
 * ## Why this file no longer touches the locale
 *
 * It used to. Two arms, chosen by a preprocessor guard: uselocale() where a
 * per-thread locale existed, and setlocale() where it did not. The guard was
 * wrong - it tested _POSIX_C_SOURCE above the first #include, and that macro
 * is defined *by* features.h, so it was false everywhere, always. The #elif
 * after it tested __APPLE__ and __FreeBSD__, which are compiler predefines
 * and so *are* live there. The chain therefore selected the good arm on the
 * two platforms nobody builds on and the process-wide arm on Linux.
 *
 * Fixing the guard was the obvious repair and it was not enough, because
 * Windows is a supported target here - the Makefile has /mingw32 and
 * /mingw64 arms with their own install paths - and MinGW has no uselocale.
 * The fallback was not covering some hypothetical platform. It was covering
 * one third of what this library ships to.
 *
 * So the locale is not consulted and not changed. Nothing here calls
 * setlocale, uselocale, newlocale or localeconv, and that is the property to
 * keep: a guard that selects between two arms can select the wrong one, and
 * this file has already proved that it will. No arms, nothing to select.
 *
 * ## What is actually locale-dependent
 *
 * Measured, not assumed, across C, de_DE.UTF-8 (separator ",") and
 * ps_AF.UTF-8 (separator U+066B, two bytes):
 *
 *   - "%lld" and "%llu" are byte-identical in all three. Integer conversion
 *     has no locale-dependent element at all; grouping would need the "'"
 *     flag, which nothing here passes.
 *   - "%.17g", "%.*f" and "%.*e" differ in exactly one place: the decimal
 *     separator. The digits stay ASCII even in a locale with digits of its
 *     own, the exponent marker and its sign do not move.
 *
 * That gives the two halves below their shape.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text_number_internal.h"

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

/* Every byte a %g, %f or %e conversion of a double can produce, except the
   decimal separator: digits, the sign, the exponent marker, and the letters
   of "inf", "infinity" and "nan" in either case. A byte outside this set is
   therefore part of the separator, whatever the separator happens to be -
   which means the repair below never has to ask the locale what it is. */
static bool is_numeric_byte(unsigned char c) {
  return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'
      || c == 'e' || c == 'E'
      || c == 'i' || c == 'I' || c == 'n' || c == 'N' || c == 'f' || c == 'F'
      || c == 'a' || c == 'A' || c == 't' || c == 'T' || c == 'y' || c == 'Y';
}

/* Rewrite whatever separator the locale used as ".", in place.
 *
 * A single conversion emits at most one separator, so there is at most one
 * run of non-numeric bytes to find. A multi-byte separator shortens the
 * string, which is why the length is returned rather than left to the caller
 * to recompute. */
static int normalise_separator(char * buf, int len) {
  int i = 0;
  while (i < len && is_numeric_byte((unsigned char)buf[i])) i++;
  if (i == len) return len; /* already "." or no fractional part at all */

  int j = i;
  while (j < len && !is_numeric_byte((unsigned char)buf[j])) j++;

  buf[i] = '.';
  if (j > i + 1) {
    memmove(buf + i + 1, buf + j, (size_t)(len - j) + 1);
    len -= (j - i - 1);
  }
  return len;
}

GTEXT_INTERNAL_API int gtext_number_format_double(
    char * buf, size_t buf_size, double value,
    GTEXT_Number_Style style, int precision) {
  if (!buf || buf_size == 0) {
    return -1;
  }
  if (precision < 0) {
    precision = 0;
  }

  const char * fmt = style == GTEXT_NUMBER_FIXED        ? "%.*f"
                   : style == GTEXT_NUMBER_SCIENTIFIC   ? "%.*e"
                                                        : "%.*g";
  int len = snprintf(buf, buf_size, fmt, precision, value);
  if (len < 0 || (size_t)len >= buf_size) {
    /* Truncated. The caller treats this as a failure, and the separator may
       not even be in the buffer, so there is nothing useful to repair. */
    return len;
  }
  return normalise_separator(buf, len);
}

GTEXT_INTERNAL_API int gtext_number_format_i64(
    char * buf, size_t buf_size, int64_t value) {
  if (!buf || buf_size == 0) {
    return -1;
  }
  /* No repair: integer conversion has no locale-dependent element. Pinned by
     LocaleNumbers.AnIntegerIsSpelledTheSameInEveryLocale. */
  return snprintf(buf, buf_size, "%lld", (long long)value);
}

GTEXT_INTERNAL_API int gtext_number_format_u64(
    char * buf, size_t buf_size, uint64_t value) {
  if (!buf || buf_size == 0) {
    return -1;
  }
  return snprintf(buf, buf_size, "%llu", (unsigned long long)value);
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static bool cs_digit(unsigned char c) {
  return c >= '0' && c <= '9';
}
static bool cs_xdigit(unsigned char c) {
  return cs_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool cs_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f'
      || c == '\r';
}
static bool cs_alpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static unsigned char cs_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

/**
 * How many bytes strtod() would consume from @p s *in the C locale*; 0 if it
 * would convert nothing.
 *
 * This is the piece that makes truncation possible, and truncation is what
 * makes the parse safe: handed "1,5" in a comma locale, a bare strtod
 * returns 1.5, which is not what the document said. Knowing the token ends
 * after "1" is what stops it.
 *
 * Held to strtod's exact grammar by LocaleNumbers.TheExtentScanAgreesWith
 * Strtod, which compares the two over four million generated inputs; three
 * deliberate mutations of the rules below were caught by it 63984, 3224 and
 * 371362 times respectively.
 */
GTEXT_INTERNAL_API size_t gtext_number_c_extent(const char * s) {
  const char * p = s;
  while (cs_space((unsigned char)*p)) p++;
  if (*p == '+' || *p == '-') p++;

  if (cs_lower((unsigned char)p[0]) == 'i'
      && cs_lower((unsigned char)p[1]) == 'n'
      && cs_lower((unsigned char)p[2]) == 'f') {
    static const char rest[] = "inity";
    const char * q = p + 3;
    size_t k = 0;
    while (rest[k] && cs_lower((unsigned char)q[k]) == (unsigned char)rest[k]) {
      k++;
    }
    return (size_t)((rest[k] == '\0' ? q + 5 : q) - s);
  }

  if (cs_lower((unsigned char)p[0]) == 'n'
      && cs_lower((unsigned char)p[1]) == 'a'
      && cs_lower((unsigned char)p[2]) == 'n') {
    const char * q = p + 3;
    if (*q == '(') {
      const char * r = q + 1;
      while (cs_alpha((unsigned char)*r) || cs_digit((unsigned char)*r)
          || *r == '_') {
        r++;
      }
      if (*r == ')') q = r + 1;
    }
    return (size_t)(q - s);
  }

  if (p[0] == '0' && cs_lower((unsigned char)p[1]) == 'x') {
    const char * q = p + 2;
    const char * int_start = q;
    bool had_int, had_frac = false;
    while (cs_xdigit((unsigned char)*q)) q++;
    had_int = (q != int_start);
    if (*q == '.') {
      const char * f = q + 1;
      while (cs_xdigit((unsigned char)*f)) f++;
      had_frac = (f != q + 1);
      if (had_int || had_frac) q = f;
    }
    if (!had_int && !had_frac) {
      /* "0x" with nothing after it: strtod takes the leading "0" and stops. */
      return (size_t)((p + 1) - s);
    }
    if (cs_lower((unsigned char)*q) == 'p') {
      const char * e = q + 1;
      if (*e == '+' || *e == '-') e++;
      if (cs_digit((unsigned char)*e)) {
        while (cs_digit((unsigned char)*e)) e++;
        q = e;
      }
    }
    return (size_t)(q - s);
  }

  {
    const char * q = p;
    const char * int_start = q;
    bool had_int, had_frac = false;
    while (cs_digit((unsigned char)*q)) q++;
    had_int = (q != int_start);
    if (*q == '.') {
      const char * f = q + 1;
      while (cs_digit((unsigned char)*f)) f++;
      had_frac = (f != q + 1);
      if (had_int || had_frac) q = f;
    }
    if (!had_int && !had_frac) return 0;
    if (cs_lower((unsigned char)*q) == 'e') {
      const char * e = q + 1;
      if (*e == '+' || *e == '-') e++;
      if (cs_digit((unsigned char)*e)) {
        while (cs_digit((unsigned char)*e)) e++;
        q = e;
      }
    }
    return (size_t)(q - s);
  }
}

/* The separator this locale's strtod expects, as bytes.
 *
 * Asked by formatting a value whose spelling is known everywhere else:
 * "%.1f" of 1.5 is the digit 1, the separator, the digit 5. localeconv()
 * would answer the same question, but it returns a pointer into storage
 * shared by the whole process, and the entire point of this file is to stop
 * number conversion from having shared state. A stack buffer has none. */
static size_t locale_separator(char * out, size_t out_size) {
  char probe[32];
  int n = snprintf(probe, sizeof probe, "%.1f", 1.5);
  if (n >= 3 && (size_t)n < sizeof probe && probe[0] == '1'
      && probe[n - 1] == '5' && (size_t)(n - 2) < out_size) {
    size_t len = (size_t)n - 2;
    memcpy(out, probe + 1, len);
    out[len] = '\0';
    return len;
  }
  out[0] = '.';
  out[1] = '\0';
  return 1;
}

GTEXT_INTERNAL_API double gtext_number_strtod(
    const GTEXT_Allocator * alloc, const char * s, char ** end) {
  if (!s) {
    if (end) *end = NULL;
    return 0.0;
  }

  const size_t extent = gtext_number_c_extent(s);

  /* Optimism, with a check that costs nothing. If strtod consumed exactly
     the token the C grammar describes, then the only locale-dependent
     character in that grammar - the separator - cannot have been read as
     anything else, and the answer is already right. In a "." locale this is
     always the path taken, and no copy is ever made. */
  const int saved_errno = errno;
  errno = 0;
  char * e;
  double value = strtod(s, &e);
  if ((size_t)(e - s) == extent) {
    const int conversion_errno = errno;
    errno = conversion_errno ? conversion_errno : saved_errno;
    if (end) *end = e;
    return value;
  }

  /* It did not, so the locale disagrees with the document about which byte
     is the separator. Respell the token the way this locale reads it, and
     cut it off at the point the C grammar ends - that second half is what
     keeps "1,5" from coming back as 1.5. */
  char sep[16];
  const size_t seplen = locale_separator(sep, sizeof sep);

  char stack_buf[128];
  char * copy = stack_buf;
  const size_t needed = extent + seplen + 1;
  if (needed > sizeof stack_buf) {
    /* Through the caller's allocator. A token has no bound on how long it may
       be *written*, so this branch is reachable, and it is the only allocation
       in this file: for as long as it came from the C library directly it was
       memory a caller who supplied an allocator could not see, in a path
       reached from seven places across the JSON, YAML, TOML and INI readers.
       It is doubly invisible - only in a locale whose separator is not "."
       and only for a token over 128 bytes - which is why it survived the
       sweep that converted the rest of the library.

       The first draft of this comment named the C function, and
       `make check-allocators` refused the file for it: that gate excludes a
       comment line only when the line *starts* with a continuation marker,
       and this file's block comments do not repeat one. Worth knowing before
       weakening the pattern to fit a sentence. */
    copy = (char *)gtext_allocator_malloc(alloc, needed);
    if (!copy) {
      /* Converting in the wrong locale would be a silently wrong number, so
         report that nothing was converted instead. */
      errno = saved_errno;
      if (end) *end = (char *)s;
      return 0.0;
    }
  }

  size_t dot = SIZE_MAX; /* index in s of the '.' that was respelled */
  size_t o = 0;
  for (size_t i = 0; i < extent; i++) {
    if (s[i] == '.' && dot == SIZE_MAX) {
      dot = i;
      memcpy(copy + o, sep, seplen);
      o += seplen;
    }
    else {
      copy[o++] = s[i];
    }
  }
  copy[o] = '\0';

  errno = 0;
  char * ce;
  value = strtod(copy, &ce);
  const int conversion_errno = errno;

  /* Map the stopping point back into the caller's string, allowing for a
     separator that is not one byte wide. */
  const size_t co = (size_t)(ce - copy);
  size_t so;
  if (dot == SIZE_MAX || co <= dot) {
    so = co;
  }
  else if (co >= dot + seplen) {
    so = co - (seplen - 1);
  }
  else {
    so = dot;
  }
  if (so > extent) so = extent;

  if (copy != stack_buf) gtext_allocator_free(alloc, copy);

  errno = conversion_errno ? conversion_errno : saved_errno;
  if (end) *end = (char *)s + so;
  return value;
}
