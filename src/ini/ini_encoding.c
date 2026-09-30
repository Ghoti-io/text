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
 * @file ini_encoding.c
 * @brief What a leading byte-order mark says, and turning UTF-16 into UTF-8.
 *
 * This module reads bytes, and everything above this file assumes the bytes it
 * is handed are the text. A Windows `.ini` is sometimes not: the `W` profile
 * entry points read a UTF-16LE file, and notepad writes one whenever it is told
 * "Unicode". Before this file existed such a document did not fail to parse -
 * it parsed into nonsense, which under ::GTEXT_INI_DIALECT_WIN32 is guaranteed
 * because that dialect refuses nothing. So the purpose here is first to make
 * the nonsense impossible and only second to read the file.
 */

#include "ini_internal.h"

GTEXT_INI_Source_Encoding gtext_ini_detect_encoding(const char * bytes,
    size_t len, size_t * bom_len) {
  if (bom_len) *bom_len = 0;
  if (!bytes) return GTEXT_INI_SOURCE_BYTES;
  const unsigned char * b = (const unsigned char *) bytes;

  /*
   * The four-byte marks come first, and that ordering is the whole correctness
   * of this function rather than a tidiness. `FF FE` opens both UTF-16LE and
   * UTF-32LE; a sniffer that tests the two-byte mark first calls every UTF-32LE
   * document UTF-16LE, and then a decoder turns it into alternating text and
   * NULs with no error anywhere. `00 00 FE FF` has no such overlap but is
   * grouped with it so that the two four-byte tests read as one decision.
   */
  if (len >= 4) {
    if (b[0] == 0xFF && b[1] == 0xFE && b[2] == 0x00 && b[3] == 0x00) {
      if (bom_len) *bom_len = 4;
      return GTEXT_INI_SOURCE_UTF32LE;
    }
    if (b[0] == 0x00 && b[1] == 0x00 && b[2] == 0xFE && b[3] == 0xFF) {
      if (bom_len) *bom_len = 4;
      return GTEXT_INI_SOURCE_UTF32BE;
    }
  }
  if (len >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
    if (bom_len) *bom_len = 3;
    return GTEXT_INI_SOURCE_UTF8;
  }
  if (len >= 2) {
    if (b[0] == 0xFF && b[1] == 0xFE) {
      if (bom_len) *bom_len = 2;
      return GTEXT_INI_SOURCE_UTF16LE;
    }
    if (b[0] == 0xFE && b[1] == 0xFF) {
      if (bom_len) *bom_len = 2;
      return GTEXT_INI_SOURCE_UTF16BE;
    }
  }
  return GTEXT_INI_SOURCE_BYTES;
}

/** Encode @p cp as UTF-8 into @p out, returning the bytes written. */
static size_t ini_utf8_put(char * out, uint32_t cp) {
  if (cp < 0x80) {
    out[0] = (char) cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char) (0xC0 | (cp >> 6));
    out[1] = (char) (0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char) (0xE0 | (cp >> 12));
    out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char) (0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char) (0xF0 | (cp >> 18));
  out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char) (0x80 | (cp & 0x3F));
  return 4;
}

bool gtext_ini_utf16_to_utf8(const GTEXT_Allocator * alloc, const char * bytes,
    size_t len, bool big_endian, char ** out, size_t * out_len,
    GTEXT_INI_Status * status, const char ** message, size_t * offset) {
  *out = NULL;
  *out_len = 0;
  *status = GTEXT_INI_OK;
  *message = NULL;
  *offset = 0;

  if (len % 2) {
    *status = GTEXT_INI_E_BAD_UNICODE;
    *message = "the UTF-16 document has an odd number of bytes";
    *offset = len - 1;
    return false;
  }

  /*
   * Four UTF-8 bytes per UTF-16 code unit is the bound, and it is loose on
   * purpose: the tight bound needs a counting pass, and a counting pass that
   * disagrees with the filling pass about one code point is the defect this
   * codebase has already met twice. One pass, one predicate, and the slack is
   * freed by the caller a moment later. The +1 is the NUL the parser's callers
   * rely on being there.
   */
  size_t units = len / 2;
  size_t cap = units * 4 + 1;
  char * buf = gtext_allocator_malloc(alloc, cap);
  if (!buf) {
    *status = GTEXT_INI_E_OOM;
    *message = "out of memory decoding UTF-16";
    return false;
  }

  const unsigned char * b = (const unsigned char *) bytes;
  size_t n = 0;
  for (size_t i = 0; i < units; i++) {
    size_t at = i * 2;
    uint32_t unit = big_endian ? (uint32_t) ((b[at] << 8) | b[at + 1])
                              : (uint32_t) ((b[at + 1] << 8) | b[at]);
    uint32_t cp = unit;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      if (i + 1 >= units) {
        gtext_allocator_free(alloc, buf);
        *status = GTEXT_INI_E_BAD_UNICODE;
        *message = "a UTF-16 high surrogate ends the document";
        *offset = at;
        return false;
      }
      size_t at2 = (i + 1) * 2;
      uint32_t low = big_endian ? (uint32_t) ((b[at2] << 8) | b[at2 + 1])
                                : (uint32_t) ((b[at2 + 1] << 8) | b[at2]);
      if (low < 0xDC00 || low > 0xDFFF) {
        gtext_allocator_free(alloc, buf);
        *status = GTEXT_INI_E_BAD_UNICODE;
        *message = "a UTF-16 high surrogate is not followed by a low one";
        *offset = at;
        return false;
      }
      cp = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
      i++;
    }
    else if (unit >= 0xDC00 && unit <= 0xDFFF) {
      /*
       * A low surrogate with no high one before it. Refused rather than passed
       * through as WTF-8, which is the choice the unicode library states for
       * every entry point it has: the caller that wants lone surrogates
       * preserved is not served by a decoder that guesses, and a Windows file
       * containing one is damaged in a way worth hearing about.
       */
      gtext_allocator_free(alloc, buf);
      *status = GTEXT_INI_E_BAD_UNICODE;
      *message = "a UTF-16 low surrogate has no high surrogate before it";
      *offset = at;
      return false;
    }
    n += ini_utf8_put(buf + n, cp);
  }
  buf[n] = 0;
  *out = buf;
  *out_len = n;
  return true;
}
