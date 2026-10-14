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
 * The decoders and encoders of the multi-byte legacy encodings: sections 10 (gb18030
 * and GBK), 11 (Big5), 12 (EUC-JP, ISO-2022-JP and Shift_JIS) and 13 (EUC-KR) of
 * the Encoding Standard.
 *
 * Each handler is the standard's algorithm transcribed step by step, in the standard's
 * order and with its names for the state ("gb18030 first", "Big5 leading", "ISO-2022-JP
 * decoder output state"), so that a reader can hold the standard beside it. What is not
 * the standard's is the data layout (tables/encoding_tables_cjk.c: forward arrays for
 * "the index code point for a pointer", reverse tables for "the index pointer for a
 * code point") and one rule that follows from the driver: a handler that returns two
 * code points returns them together (the driver writes both or neither).
 *
 * Where the standard says "restore byte to ioQueue" the handler asks the driver to put
 * the byte(s) back in front of the input (GTEXT_Dec_Step::restore); where it says
 * "restore codePoint to ioQueue" an encoder does the same with the code point
 * (GTEXT_Enc_Step::restore, restore_cp).
 */

#include <ghoti.io/text/encoding.h>

#include "encoding_internal.h"

#include <stdint.h>

/* ---- shared ------------------------------------------------------------------------------- */

static void init_zero(GTEXT_Dec_State * state, GTEXT_Encoding encoding) {
  (void)encoding;
  memset(state, 0, sizeof(*state));
}

/// "The index code point for pointer in index jis0208": the code point, or -1 for null.
static int32_t jis0208_code_point(uint32_t pointer) {
  if (pointer >= GTEXT_JIS0208_POINTERS) {
    return -1;
  }
  const uint16_t cp = gtext_encoding_jis0208_decode[pointer];
  return cp == GTEXT_INDEX_NONE ? -1 : (int32_t)cp;
}

static inline bool in_range(int byte, int lo, int hi) {
  return byte >= lo && byte <= hi;
}

static inline void emit1(GTEXT_Dec_Step * out, uint32_t cp) {
  out->cp[0] = cp;
  out->n = 1;
}

/* ---- gb18030 and GBK (section 10) ------------------------------------------------------------- */

/// The index gb18030 ranges code point for a pointer: the code point, or -1 for null.
static int64_t gb18030_ranges_code_point(uint32_t pointer) {
  /* If pointer is greater than 39419 and less than 189000, or pointer is greater than 1237575, then return null. */
  if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) {
    return -1;
  }
  /* If pointer is 7457, then return code point U+E7C7. */
  if (pointer == 7457) {
    return 0xE7C7;
  }
  /* Let offset be the last pointer in index gb18030 ranges that is less than or equal to pointer and let
     codePointOffset be its corresponding code point. */
  size_t lo = 0;
  size_t hi = GTEXT_GB18030_RANGES; /* the first row whose pointer is greater */
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (gtext_encoding_gb18030_ranges_pointer[mid] <= pointer) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  /* (pointer 0 is the first row's, so lo is at least 1) */
  const size_t row = lo - 1;
  /* Return a code point whose value is codePointOffset + pointer - offset. */
  return (int64_t)gtext_encoding_gb18030_ranges_cp[row] + (int64_t)pointer - (int64_t)gtext_encoding_gb18030_ranges_pointer[row];
}

/// The index gb18030 ranges pointer for a code point.
static uint32_t gb18030_ranges_pointer(uint32_t cp) {
  /* If codePoint is U+E7C7, then return pointer 7457. */
  if (cp == 0xE7C7) {
    return 7457;
  }
  /* Let offset be the last code point in index gb18030 ranges that is less than or equal to codePoint and let
     pointerOffset be its corresponding pointer. */
  size_t lo = 0;
  size_t hi = GTEXT_GB18030_RANGES;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (gtext_encoding_gb18030_ranges_cp[mid] <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  const size_t row = lo - 1; /* the first row's code point is 0 */
  /* Return a pointer whose value is pointerOffset + codePoint - offset. */
  return gtext_encoding_gb18030_ranges_pointer[row] + cp - gtext_encoding_gb18030_ranges_cp[row];
}

static bool gb18030_idle(const GTEXT_Dec_State * state) {
  return state->gb18030.first == 0;
}

/// gb18030's decoder's handler (10.2.1); GBK's decoder is the same ("GBK's decoder is gb18030's decoder").
static GTEXT_Dec_Result gb18030_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  GTEXT_Gb18030_State * s = &state->gb18030;
  /* If byte is end-of-queue and gb18030 first, second, and third are 0x00, then return finished. */
  if (byte < 0 && s->first == 0 && s->second == 0 && s->third == 0) {
    return GTEXT_DEC_FINISHED;
  }
  /* If byte is end-of-queue, and gb18030 first, second, or third is not 0x00, then set them to 0x00, and return error. */
  if (byte < 0) {
    s->first = s->second = s->third = 0;
    return GTEXT_DEC_ERROR;
  }
  /* If gb18030 third is not 0x00: */
  if (s->third != 0) {
    /* If byte is not in the range 0x30 to 0x39, inclusive: restore « second, third, byte », set first, second and third to 0x00, return error. */
    if (!in_range(byte, 0x30, 0x39)) {
      out->restore_n = 3;
      out->restore[0] = s->second;
      out->restore[1] = s->third;
      out->restore[2] = (uint8_t)byte;
      s->first = s->second = s->third = 0;
      return GTEXT_DEC_ERROR;
    }
    /* Let codePoint be the index gb18030 ranges code point for
       ((first - 0x81) x (10 x 126 x 10)) + ((second - 0x30) x (10 x 126)) + ((third - 0x81) x 10) + byte - 0x30. */
    const uint32_t pointer = ((uint32_t)s->first - 0x81) * (10 * 126 * 10) + ((uint32_t)s->second - 0x30) * (10 * 126)
        + ((uint32_t)s->third - 0x81) * 10 + (uint32_t)byte - 0x30;
    const int64_t cp = gb18030_ranges_code_point(pointer);
    /* Set gb18030 first, second, and third to 0x00. */
    s->first = s->second = s->third = 0;
    /* If codePoint is null, then return error. */
    if (cp < 0) {
      return GTEXT_DEC_ERROR;
    }
    emit1(out, (uint32_t)cp);
    return GTEXT_DEC_EMIT;
  }
  /* If gb18030 second is not 0x00: */
  if (s->second != 0) {
    /* If byte is in the range 0x81 to 0xFE, inclusive, then set gb18030 third to byte and return continue. */
    if (in_range(byte, 0x81, 0xFE)) {
      s->third = (uint8_t)byte;
      return GTEXT_DEC_CONTINUE;
    }
    /* Restore « gb18030 second, byte » to ioQueue, set gb18030 first and gb18030 second to 0x00, and return error. */
    out->restore_n = 2;
    out->restore[0] = s->second;
    out->restore[1] = (uint8_t)byte;
    s->first = s->second = 0;
    return GTEXT_DEC_ERROR;
  }
  /* If gb18030 first is not 0x00: */
  if (s->first != 0) {
    /* If byte is in the range 0x30 to 0x39, inclusive, then set gb18030 second to byte and return continue. */
    if (in_range(byte, 0x30, 0x39)) {
      s->second = (uint8_t)byte;
      return GTEXT_DEC_CONTINUE;
    }
    /* Let leading be gb18030 first. Set gb18030 first to 0x00. Let pointer be null. */
    const uint32_t leading = s->first;
    s->first = 0;
    int64_t pointer = -1;
    /* Let offset be 0x40 if byte is less than 0x7F; otherwise 0x41. */
    const int offset = byte < 0x7F ? 0x40 : 0x41;
    /* If byte is in the range 0x40 to 0x7E, inclusive, or 0x80 to 0xFE, inclusive, then set pointer to (leading - 0x81) x 190 + (byte - offset). */
    if (in_range(byte, 0x40, 0x7E) || in_range(byte, 0x80, 0xFE)) {
      pointer = (int64_t)(leading - 0x81) * 190 + (byte - offset);
    }
    /* Let codePoint be null if pointer is null; otherwise the index code point for pointer in index gb18030. */
    if (pointer >= 0 && pointer < GTEXT_GB18030_POINTERS) {
      const uint16_t cp = gtext_encoding_gb18030_decode[pointer];
      /* If codePoint is non-null, then return a code point whose value is codePoint. */
      if (cp != GTEXT_INDEX_NONE) {
        emit1(out, cp);
        return GTEXT_DEC_EMIT;
      }
    }
    /* If byte is an ASCII byte, then restore byte to ioQueue. */
    if (byte <= 0x7F) {
      out->restore_n = 1;
      out->restore[0] = (uint8_t)byte;
    }
    /* Return error. */
    return GTEXT_DEC_ERROR;
  }
  /* If byte is an ASCII byte, then return a code point whose value is byte. */
  if (byte <= 0x7F) {
    emit1(out, (uint32_t)byte);
    return GTEXT_DEC_EMIT;
  }
  /* If byte is 0x80, then return code point U+20AC (euro sign). */
  if (byte == 0x80) {
    emit1(out, 0x20AC);
    return GTEXT_DEC_EMIT;
  }
  /* If byte is in the range 0x81 to 0xFE, inclusive, then set gb18030 first to byte and return continue. */
  if (in_range(byte, 0x81, 0xFE)) {
    s->first = (uint8_t)byte;
    return GTEXT_DEC_CONTINUE;
  }
  /* Return error. */
  return GTEXT_DEC_ERROR;
}

/// The asymmetric part of gb18030's encoder (10.2.2): code points the index does not map back, for compatibility with GB18030-2005.
static const struct {
  uint16_t cp;
  uint8_t b1;
  uint8_t b2;
} gb18030_side[] = {
  {0xE78D, 0xA6, 0xD9}, {0xE78E, 0xA6, 0xDA}, {0xE78F, 0xA6, 0xDB}, {0xE790, 0xA6, 0xDC}, {0xE791, 0xA6, 0xDD},
  {0xE792, 0xA6, 0xDE}, {0xE793, 0xA6, 0xDF}, {0xE794, 0xA6, 0xEC}, {0xE795, 0xA6, 0xED}, {0xE796, 0xA6, 0xF3},
  {0xE81E, 0xFE, 0x59}, {0xE826, 0xFE, 0x61}, {0xE82B, 0xFE, 0x66}, {0xE82C, 0xFE, 0x67}, {0xE832, 0xFE, 0x6D},
  {0xE843, 0xFE, 0x7E}, {0xE854, 0xFE, 0x90}, {0xE864, 0xFE, 0xA0},
};

/// gb18030's encoder's handler (10.2.2), with "is GBK" as `is_gbk`.
static GTEXT_Enc_Result gb18030_encode(int32_t cp, GTEXT_Enc_Step * out, bool is_gbk) {
  /* If codePoint is end-of-queue, then return finished. */
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  /* If codePoint is an ASCII code point, then return a byte whose value is codePoint. */
  if (cp <= 0x7F) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is U+E5E5, then return error with codePoint. */
  if (cp == 0xE5E5) {
    return GTEXT_ENC_ERROR;
  }
  /* If is GBK is true and codePoint is U+20AC (euro sign), then return byte 0x80. */
  if (is_gbk && cp == 0x20AC) {
    out->bytes[0] = 0x80;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If there is a row in the table whose first column is codePoint, then return the two bytes on the same row. */
  for (size_t i = 0; i < sizeof(gb18030_side) / sizeof(gb18030_side[0]); i++) {
    if (gb18030_side[i].cp == cp) {
      out->bytes[0] = gb18030_side[i].b1;
      out->bytes[1] = gb18030_side[i].b2;
      out->n = 2;
      return GTEXT_ENC_OK;
    }
  }
  /* Let pointer be the index pointer for codePoint in index gb18030. If pointer is non-null: */
  const uint32_t pointer = gtext_rev_lookup(&gtext_encoding_gb18030_rev, (uint32_t)cp);
  if (pointer != GTEXT_INDEX_NONE) {
    /* Let leading be pointer / 190 + 0x81. Let trailing be pointer % 190. Let offset be 0x40 if trailing is less than 0x3F, otherwise 0x41. */
    const uint32_t trailing = pointer % 190;
    const uint32_t offset = trailing < 0x3F ? 0x40 : 0x41;
    out->bytes[0] = (uint8_t)(pointer / 190 + 0x81);
    out->bytes[1] = (uint8_t)(trailing + offset);
    out->n = 2;
    return GTEXT_ENC_OK;
  }
  /* If is GBK is true, then return error with codePoint. */
  if (is_gbk) {
    return GTEXT_ENC_ERROR;
  }
  /* Set pointer to the index gb18030 ranges pointer for codePoint. */
  uint32_t p = gb18030_ranges_pointer((uint32_t)cp);
  /* byte1 = pointer / (10 x 126 x 10); pointer %= ...; byte2 = pointer / (10 x 126); pointer %= ...; byte3 = pointer / 10; byte4 = pointer % 10. */
  const uint32_t byte1 = p / (10 * 126 * 10);
  p %= 10 * 126 * 10;
  const uint32_t byte2 = p / (10 * 126);
  p %= 10 * 126;
  const uint32_t byte3 = p / 10;
  const uint32_t byte4 = p % 10;
  out->bytes[0] = (uint8_t)(byte1 + 0x81);
  out->bytes[1] = (uint8_t)(byte2 + 0x30);
  out->bytes[2] = (uint8_t)(byte3 + 0x81);
  out->bytes[3] = (uint8_t)(byte4 + 0x30);
  out->n = 4;
  return GTEXT_ENC_OK;
}

static GTEXT_Enc_Result gb18030_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  (void)state;
  return gb18030_encode(cp, out, false);
}

static GTEXT_Enc_Result gbk_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  (void)state;
  return gb18030_encode(cp, out, true);
}

/* ---- Big5 (section 11) ------------------------------------------------------------------------- */

static bool lead_idle(const GTEXT_Dec_State * state) {
  return state->lead.leading == 0;
}

/// Big5's decoder's handler (11.1.1).
static GTEXT_Dec_Result big5_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  GTEXT_Lead_State * s = &state->lead;
  /* If byte is end-of-queue and Big5 leading is not 0x00, then set Big5 leading to 0x00 and return error. */
  if (byte < 0 && s->leading != 0) {
    s->leading = 0;
    return GTEXT_DEC_ERROR;
  }
  /* If byte is end-of-queue and Big5 leading is 0x00, then return finished. */
  if (byte < 0) {
    return GTEXT_DEC_FINISHED;
  }
  /* If Big5 leading is not 0x00: */
  if (s->leading != 0) {
    /* Let leading be Big5 leading. Set Big5 leading to 0x00. Let pointer be null. */
    const uint32_t leading = s->leading;
    s->leading = 0;
    int64_t pointer = -1;
    /* Let offset be 0x40 if byte is less than 0x7F; otherwise 0x62. */
    const int offset = byte < 0x7F ? 0x40 : 0x62;
    /* If byte is in the range 0x40 to 0x7E, inclusive, or 0xA1 to 0xFE, inclusive, then set pointer to (leading - 0x81) x 157 + (byte - offset). */
    if (in_range(byte, 0x40, 0x7E) || in_range(byte, 0xA1, 0xFE)) {
      pointer = (int64_t)(leading - 0x81) * 157 + (byte - offset);
    }
    /* If there is a row in the table below whose first column is pointer, then return the two code points listed in its second column. */
    switch (pointer) {
    case 1133:
      out->cp[0] = 0x00CA;
      out->cp[1] = 0x0304;
      out->n = 2;
      return GTEXT_DEC_EMIT;
    case 1135:
      out->cp[0] = 0x00CA;
      out->cp[1] = 0x030C;
      out->n = 2;
      return GTEXT_DEC_EMIT;
    case 1164:
      out->cp[0] = 0x00EA;
      out->cp[1] = 0x0304;
      out->n = 2;
      return GTEXT_DEC_EMIT;
    case 1166:
      out->cp[0] = 0x00EA;
      out->cp[1] = 0x030C;
      out->n = 2;
      return GTEXT_DEC_EMIT;
    default:
      break;
    }
    /* Let codePoint be null if pointer is null; otherwise the index code point for pointer in index Big5. */
    if (pointer >= 0 && pointer < GTEXT_BIG5_POINTERS) {
      const uint16_t lo = gtext_encoding_big5_decode[pointer];
      const bool plane2 = (gtext_encoding_big5_plane2[pointer >> 3] >> (pointer & 7)) & 1;
      /* If codePoint is non-null, then return a code point whose value is codePoint. */
      if (lo != GTEXT_INDEX_NONE || plane2) {
        emit1(out, (uint32_t)lo + (plane2 ? 0x20000u : 0u));
        return GTEXT_DEC_EMIT;
      }
    }
    /* If byte is an ASCII byte, restore byte to ioQueue. */
    if (byte <= 0x7F) {
      out->restore_n = 1;
      out->restore[0] = (uint8_t)byte;
    }
    /* Return error. */
    return GTEXT_DEC_ERROR;
  }
  /* If byte is an ASCII byte, then return a code point whose value is byte. */
  if (byte <= 0x7F) {
    emit1(out, (uint32_t)byte);
    return GTEXT_DEC_EMIT;
  }
  /* If byte is in the range 0x81 to 0xFE, inclusive, then set Big5 leading to byte and return continue. */
  if (in_range(byte, 0x81, 0xFE)) {
    s->leading = (uint8_t)byte;
    return GTEXT_DEC_CONTINUE;
  }
  /* Return error. */
  return GTEXT_DEC_ERROR;
}

/// Big5's encoder's handler (11.1.2); "the index Big5 pointer for codePoint" is the reverse table built as that algorithm says.
static GTEXT_Enc_Result big5_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  (void)state;
  /* If codePoint is end-of-queue, then return finished. */
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  /* If codePoint is an ASCII code point, then return a byte whose value is codePoint. */
  if (cp <= 0x7F) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* Let pointer be the index Big5 pointer for codePoint. If pointer is null, then return error with codePoint. */
  const uint32_t pointer = gtext_rev_lookup(&gtext_encoding_big5_rev, (uint32_t)cp);
  if (pointer == GTEXT_INDEX_NONE) {
    return GTEXT_ENC_ERROR;
  }
  /* Let leading be pointer / 157 + 0x81. Let trailing be pointer % 157. Let offset be 0x40 if trailing is less than 0x3F, otherwise 0x62. */
  const uint32_t trailing = pointer % 157;
  const uint32_t offset = trailing < 0x3F ? 0x40 : 0x62;
  out->bytes[0] = (uint8_t)(pointer / 157 + 0x81);
  out->bytes[1] = (uint8_t)(trailing + offset);
  out->n = 2;
  return GTEXT_ENC_OK;
}

/* ---- EUC-JP (section 12.1) ------------------------------------------------------------------------ */

/// EUC-JP's decoder's handler (12.1.1). `lead.leading` is "EUC-JP leading" and `lead.jis0212` is "EUC-JP jis0212".
static GTEXT_Dec_Result euc_jp_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  GTEXT_Lead_State * s = &state->lead;
  /* If byte is end-of-queue and EUC-JP leading is not 0x00, then set EUC-JP leading to 0x00 and return error. */
  if (byte < 0 && s->leading != 0) {
    s->leading = 0;
    return GTEXT_DEC_ERROR;
  }
  /* If byte is end-of-queue and EUC-JP leading is 0x00, then return finished. */
  if (byte < 0) {
    return GTEXT_DEC_FINISHED;
  }
  /* If EUC-JP leading is 0x8E and byte is in the range 0xA1 to 0xDF, inclusive, then set EUC-JP leading to 0x00 and
     return a code point whose value is 0xFF61 - 0xA1 + byte. */
  if (s->leading == 0x8E && in_range(byte, 0xA1, 0xDF)) {
    s->leading = 0;
    emit1(out, 0xFF61u - 0xA1u + (uint32_t)byte);
    return GTEXT_DEC_EMIT;
  }
  /* If EUC-JP leading is 0x8F and byte is in the range 0xA1 to 0xFE, inclusive, then set EUC-JP jis0212 to true,
     set EUC-JP leading to byte, and return continue. */
  if (s->leading == 0x8F && in_range(byte, 0xA1, 0xFE)) {
    s->jis0212 = true;
    s->leading = (uint8_t)byte;
    return GTEXT_DEC_CONTINUE;
  }
  /* If EUC-JP leading is not 0x00: */
  if (s->leading != 0) {
    /* Let leading be EUC-JP leading. Set EUC-JP leading to 0x00. Let codePoint be null. */
    const uint32_t leading = s->leading;
    s->leading = 0;
    int32_t cp = -1;
    /* If leading and byte are both in the range 0xA1 to 0xFE, inclusive, then set codePoint to the index code point for
       (leading - 0xA1) x 94 + byte - 0xA1 in index jis0208 if EUC-JP jis0212 is false and in index jis0212 otherwise. */
    if (in_range((int)leading, 0xA1, 0xFE) && in_range(byte, 0xA1, 0xFE)) {
      const uint32_t pointer = (leading - 0xA1) * 94 + (uint32_t)byte - 0xA1;
      if (s->jis0212) {
        if (pointer < GTEXT_JIS0212_POINTERS && gtext_encoding_jis0212_decode[pointer] != GTEXT_INDEX_NONE) {
          cp = gtext_encoding_jis0212_decode[pointer];
        }
      } else {
        cp = jis0208_code_point(pointer);
      }
    }
    /* Set EUC-JP jis0212 to false. */
    s->jis0212 = false;
    /* If codePoint is non-null, then return a code point whose value is codePoint. */
    if (cp >= 0) {
      emit1(out, (uint32_t)cp);
      return GTEXT_DEC_EMIT;
    }
    /* If byte is an ASCII byte, then restore byte to ioQueue. */
    if (byte <= 0x7F) {
      out->restore_n = 1;
      out->restore[0] = (uint8_t)byte;
    }
    /* Return error. */
    return GTEXT_DEC_ERROR;
  }
  /* If byte is an ASCII byte, then return a code point whose value is byte. */
  if (byte <= 0x7F) {
    emit1(out, (uint32_t)byte);
    return GTEXT_DEC_EMIT;
  }
  /* If byte is 0x8E, 0x8F, or in the range 0xA1 to 0xFE, inclusive, then set EUC-JP leading to byte and return continue. */
  if (byte == 0x8E || byte == 0x8F || in_range(byte, 0xA1, 0xFE)) {
    s->leading = (uint8_t)byte;
    return GTEXT_DEC_CONTINUE;
  }
  /* Return error. */
  return GTEXT_DEC_ERROR;
}

/// EUC-JP's encoder's handler (12.1.2).
static GTEXT_Enc_Result euc_jp_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  (void)state;
  /* If codePoint is end-of-queue, then return finished. */
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  /* If codePoint is an ASCII code point, then return a byte whose value is codePoint. */
  if (cp <= 0x7F) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is U+00A5, then return byte 0x5C. */
  if (cp == 0x00A5) {
    out->bytes[0] = 0x5C;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is U+203E, then return byte 0x7E. */
  if (cp == 0x203E) {
    out->bytes[0] = 0x7E;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is in the range U+FF61 to U+FF9F, inclusive, then return two bytes whose values are 0x8E and codePoint - 0xFF61 + 0xA1. */
  if (cp >= 0xFF61 && cp <= 0xFF9F) {
    out->bytes[0] = 0x8E;
    out->bytes[1] = (uint8_t)(cp - 0xFF61 + 0xA1);
    out->n = 2;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is U+2212, then set it to U+FF0D. */
  if (cp == 0x2212) {
    cp = 0xFF0D;
  }
  /* Let pointer be the index pointer for codePoint in index jis0208. If pointer is null, then return error with codePoint. */
  const uint32_t pointer = gtext_rev_lookup(&gtext_encoding_jis0208_rev, (uint32_t)cp);
  if (pointer == GTEXT_INDEX_NONE) {
    out->error_cp = (uint32_t)cp;
    return GTEXT_ENC_ERROR;
  }
  /* Let leading be pointer / 94 + 0xA1. Let trailing be pointer % 94 + 0xA1. Return two bytes. */
  out->bytes[0] = (uint8_t)(pointer / 94 + 0xA1);
  out->bytes[1] = (uint8_t)(pointer % 94 + 0xA1);
  out->n = 2;
  return GTEXT_ENC_OK;
}

/* ---- ISO-2022-JP (section 12.2) ------------------------------------------------------------------- */

/// ISO-2022-JP's decoder states (12.2.1), and its encoder's three states in the first three.
enum {
  ISO_ASCII = 0,
  ISO_ROMAN,
  ISO_KATAKANA,
  ISO_LEAD_BYTE,
  ISO_TRAIL_BYTE,
  ISO_ESCAPE_START,
  ISO_ESCAPE
};

static void iso_2022_jp_dec_init(GTEXT_Dec_State * state, GTEXT_Encoding encoding) {
  (void)encoding;
  memset(state, 0, sizeof(*state));
  state->iso2022jp.state = ISO_ASCII;
  state->iso2022jp.output_state = ISO_ASCII;
}

/// What of an ISO-2022-JP decoder survives a fatal error in a call that did not flush (see gtext_decoder_decode()): the **mode** the last escape sequence
/// selected, which is the decoder output state (ASCII, Roman, katakana or JIS X 0208 - the "leading byte" state). The standard's own error paths already return
/// every other state to it ("Set the ISO-2022-JP decoder state to leading byte" after a bad trailing byte; "to ISO-2022-JP decoder output state" after an escape
/// that was not one); what could be left over is a half-read escape sequence or leading byte (a trailing byte followed by 0x1B), the leading-byte value of an
/// escape, and "output" set by an escape sequence that was itself the error (two with nothing between). None of those belongs to the next stream, which would
/// otherwise start by finishing the last one's escape or be told that its first escape follows another.
void gtext_encoding_iso_2022_jp_dec_resume(GTEXT_Dec_State * state) {
  GTEXT_Iso2022jp_State * s = &state->iso2022jp;
  s->state = s->output_state;
  s->leading = 0;
  s->output = false;
}

/// ISO-2022-JP's decoder's handler (12.2.1), switching on the decoder state.
static GTEXT_Dec_Result iso_2022_jp_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  GTEXT_Iso2022jp_State * s = &state->iso2022jp;
  switch (s->state) {
  case ISO_ASCII:
    if (byte == 0x1B) { /* Set decoder state to escape start and return continue. */
      s->state = ISO_ESCAPE_START;
      return GTEXT_DEC_CONTINUE;
    }
    if (byte >= 0 && byte <= 0x7F && byte != 0x0E && byte != 0x0F) { /* Set output to false and return a code point whose value is byte. */
      s->output = false;
      emit1(out, (uint32_t)byte);
      return GTEXT_DEC_EMIT;
    }
    if (byte < 0) { /* end-of-queue: return finished. */
      return GTEXT_DEC_FINISHED;
    }
    s->output = false; /* Otherwise: set output to false and return error. */
    return GTEXT_DEC_ERROR;
  case ISO_ROMAN:
    if (byte == 0x1B) {
      s->state = ISO_ESCAPE_START;
      return GTEXT_DEC_CONTINUE;
    }
    if (byte == 0x5C) { /* Set output to false and return code point U+00A5. */
      s->output = false;
      emit1(out, 0x00A5);
      return GTEXT_DEC_EMIT;
    }
    if (byte == 0x7E) { /* ... U+203E. */
      s->output = false;
      emit1(out, 0x203E);
      return GTEXT_DEC_EMIT;
    }
    if (byte >= 0 && byte <= 0x7F && byte != 0x0E && byte != 0x0F) {
      s->output = false;
      emit1(out, (uint32_t)byte);
      return GTEXT_DEC_EMIT;
    }
    if (byte < 0) {
      return GTEXT_DEC_FINISHED;
    }
    s->output = false;
    return GTEXT_DEC_ERROR;
  case ISO_KATAKANA:
    if (byte == 0x1B) {
      s->state = ISO_ESCAPE_START;
      return GTEXT_DEC_CONTINUE;
    }
    if (in_range(byte, 0x21, 0x5F)) { /* Set output to false and return a code point whose value is 0xFF61 - 0x21 + byte. */
      s->output = false;
      emit1(out, 0xFF61u - 0x21u + (uint32_t)byte);
      return GTEXT_DEC_EMIT;
    }
    if (byte < 0) {
      return GTEXT_DEC_FINISHED;
    }
    s->output = false;
    return GTEXT_DEC_ERROR;
  case ISO_LEAD_BYTE:
    if (byte == 0x1B) {
      s->state = ISO_ESCAPE_START;
      return GTEXT_DEC_CONTINUE;
    }
    if (in_range(byte, 0x21, 0x7E)) { /* Set output to false, leading to byte, decoder state to trailing byte, and return continue. */
      s->output = false;
      s->leading = (uint8_t)byte;
      s->state = ISO_TRAIL_BYTE;
      return GTEXT_DEC_CONTINUE;
    }
    if (byte < 0) {
      return GTEXT_DEC_FINISHED;
    }
    s->output = false;
    return GTEXT_DEC_ERROR;
  case ISO_TRAIL_BYTE:
    if (byte == 0x1B) { /* Set decoder state to escape start and return error. */
      s->state = ISO_ESCAPE_START;
      return GTEXT_DEC_ERROR;
    }
    if (in_range(byte, 0x21, 0x7E)) {
      s->state = ISO_LEAD_BYTE;
      /* Let pointer be (ISO-2022-JP leading - 0x21) x 94 + byte - 0x21. Let codePoint be the index code point for pointer in index jis0208. */
      const int32_t cp = jis0208_code_point(((uint32_t)s->leading - 0x21) * 94 + (uint32_t)byte - 0x21);
      if (cp < 0) { /* If codePoint is null, then return error. */
        return GTEXT_DEC_ERROR;
      }
      emit1(out, (uint32_t)cp);
      return GTEXT_DEC_EMIT;
    }
    /* end-of-queue, and otherwise: set the decoder state to leading byte and return error. */
    s->state = ISO_LEAD_BYTE;
    return GTEXT_DEC_ERROR;
  case ISO_ESCAPE_START:
    /* If byte is either 0x24 or 0x28, then set leading to byte, decoder state to escape, and return continue. */
    if (byte == 0x24 || byte == 0x28) {
      s->leading = (uint8_t)byte;
      s->state = ISO_ESCAPE;
      return GTEXT_DEC_CONTINUE;
    }
    /* If byte is not end-of-queue, then restore byte to ioQueue. */
    if (byte >= 0) {
      out->restore_n = 1;
      out->restore[0] = (uint8_t)byte;
    }
    /* Set output to false, decoder state to the decoder output state, and return error. */
    s->output = false;
    s->state = s->output_state;
    return GTEXT_DEC_ERROR;
  default: { /* ISO_ESCAPE */
    /* Let leading be ISO-2022-JP leading and set ISO-2022-JP leading to 0x00. Let state be null. */
    const int leading = s->leading;
    s->leading = 0;
    int next = -1;
    if (leading == 0x28 && byte == 0x42) {
      next = ISO_ASCII;
    }
    if (leading == 0x28 && byte == 0x4A) {
      next = ISO_ROMAN;
    }
    if (leading == 0x28 && byte == 0x49) {
      next = ISO_KATAKANA;
    }
    if (leading == 0x24 && (byte == 0x40 || byte == 0x42)) {
      next = ISO_LEAD_BYTE;
    }
    /* If state is non-null: */
    if (next >= 0) {
      /* Set the decoder state and the decoder output state to state. Let output be the value of ISO-2022-JP output. Set it to true. */
      s->state = (uint8_t)next;
      s->output_state = (uint8_t)next;
      const bool output = s->output;
      s->output = true;
      /* Return continue, if output is false, and error otherwise. */
      out->skip = true; /* (the escape sequence is not part of the character after it: the source map) */
      return output ? GTEXT_DEC_ERROR : GTEXT_DEC_CONTINUE;
    }
    /* If byte is end-of-queue, then restore leading to ioQueue; otherwise, restore « leading, byte » to ioQueue. */
    if (byte < 0) {
      out->restore_n = 1;
      out->restore[0] = (uint8_t)leading;
    } else {
      out->restore_n = 2;
      out->restore[0] = (uint8_t)leading;
      out->restore[1] = (uint8_t)byte;
    }
    /* Set output to false, the decoder state to the decoder output state and return error. */
    s->output = false;
    s->state = s->output_state;
    return GTEXT_DEC_ERROR;
  }
  }
}

/// ISO-2022-JP's encoder's handler (12.2.2). The encoder state (ASCII, Roman or jis0208) is `state->reserved[0]`, as ISO_ASCII, ISO_ROMAN and 2.
#define ISO_JIS0208 2u

static void iso_2022_jp_enc_init(GTEXT_Enc_State * state, GTEXT_Encoding encoding) {
  (void)encoding;
  memset(state, 0, sizeof(*state));
  state->reserved[0] = ISO_ASCII;
}

/// The three bytes of an escape sequence: 0x1B a b.
static void iso_escape(GTEXT_Enc_Step * out, uint8_t a, uint8_t b) {
  out->bytes[0] = 0x1B;
  out->bytes[1] = a;
  out->bytes[2] = b;
  out->n = 3;
}

static GTEXT_Enc_Result iso_2022_jp_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  uint32_t * mode = &state->reserved[0];
  /* If codePoint is end-of-queue and the encoder state is not ASCII, then set it to ASCII and return three bytes 0x1B 0x28 0x42. */
  if (cp < 0 && *mode != ISO_ASCII) {
    *mode = ISO_ASCII;
    iso_escape(out, 0x28, 0x42);
    return GTEXT_ENC_OK;
  }
  /* If codePoint is end-of-queue and the encoder state is ASCII, then return finished. */
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  /* If the encoder state is ASCII or Roman, and codePoint is U+000E, U+000F, or U+001B, then return error with U+FFFD. */
  if ((*mode == ISO_ASCII || *mode == ISO_ROMAN) && (cp == 0x0E || cp == 0x0F || cp == 0x1B)) {
    out->error_cp = 0xFFFD;
    return GTEXT_ENC_ERROR;
  }
  /* If the encoder state is ASCII and codePoint is an ASCII code point, then return a byte whose value is codePoint. */
  if (*mode == ISO_ASCII && cp <= 0x7F) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If the encoder state is Roman and codePoint is an ASCII code point, excluding U+005C and U+007E, or is U+00A5 or U+203E: */
  if (*mode == ISO_ROMAN && ((cp <= 0x7F && cp != 0x5C && cp != 0x7E) || cp == 0x00A5 || cp == 0x203E)) {
    out->bytes[0] = cp <= 0x7F ? (uint8_t)cp : (cp == 0x00A5 ? 0x5C : 0x7E);
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is an ASCII code point, and the encoder state is not ASCII, then restore codePoint to ioQueue, set the
     encoder state to ASCII, and return three bytes 0x1B 0x28 0x42. */
  if (cp <= 0x7F && *mode != ISO_ASCII) {
    out->restore = true;
    out->restore_cp = (uint32_t)cp;
    *mode = ISO_ASCII;
    iso_escape(out, 0x28, 0x42);
    return GTEXT_ENC_OK;
  }
  /* If codePoint is either U+00A5 or U+203E, and the encoder state is not Roman, then restore codePoint to ioQueue, set the
     encoder state to Roman, and return three bytes 0x1B 0x28 0x4A. */
  if ((cp == 0x00A5 || cp == 0x203E) && *mode != ISO_ROMAN) {
    out->restore = true;
    out->restore_cp = (uint32_t)cp;
    *mode = ISO_ROMAN;
    iso_escape(out, 0x28, 0x4A);
    return GTEXT_ENC_OK;
  }
  /* If codePoint is U+2212, then set it to U+FF0D. */
  if (cp == 0x2212) {
    cp = 0xFF0D;
  }
  /* If codePoint is in the range U+FF61 to U+FF9F, inclusive, then set it to the index code point for codePoint - 0xFF61 in index ISO-2022-JP katakana. */
  if (cp >= 0xFF61 && cp <= 0xFF9F) {
    cp = gtext_encoding_iso_2022_jp_katakana_decode[cp - 0xFF61];
  }
  /* Let pointer be the index pointer for codePoint in index jis0208. */
  const uint32_t pointer = gtext_rev_lookup(&gtext_encoding_jis0208_rev, (uint32_t)cp);
  /* If pointer is null: */
  if (pointer == GTEXT_INDEX_NONE) {
    /* If the encoder state is jis0208, then restore codePoint to ioQueue, set the encoder state to ASCII, and return three bytes 0x1B 0x28 0x42. */
    if (*mode == ISO_JIS0208) {
      out->restore = true;
      out->restore_cp = (uint32_t)cp;
      *mode = ISO_ASCII;
      iso_escape(out, 0x28, 0x42);
      return GTEXT_ENC_OK;
    }
    /* Return error with codePoint. */
    out->error_cp = (uint32_t)cp;
    return GTEXT_ENC_ERROR;
  }
  /* If the encoder state is not jis0208, then restore codePoint to ioQueue, set the encoder state to jis0208, and return three bytes 0x1B 0x24 0x42. */
  if (*mode != ISO_JIS0208) {
    out->restore = true;
    out->restore_cp = (uint32_t)cp;
    *mode = ISO_JIS0208;
    iso_escape(out, 0x24, 0x42);
    return GTEXT_ENC_OK;
  }
  /* Let leading be pointer / 94 + 0x21. Let trailing be pointer % 94 + 0x21. Return two bytes. */
  out->bytes[0] = (uint8_t)(pointer / 94 + 0x21);
  out->bytes[1] = (uint8_t)(pointer % 94 + 0x21);
  out->n = 2;
  return GTEXT_ENC_OK;
}

/* ---- Shift_JIS (section 12.3) ----------------------------------------------------------------------- */

/// Shift_JIS's decoder's handler (12.3.1).
static GTEXT_Dec_Result shift_jis_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  GTEXT_Lead_State * s = &state->lead;
  /* If byte is end-of-queue and Shift_JIS leading is not 0x00, then set Shift_JIS leading to 0x00 and return error. */
  if (byte < 0 && s->leading != 0) {
    s->leading = 0;
    return GTEXT_DEC_ERROR;
  }
  /* If byte is end-of-queue and Shift_JIS leading is 0x00, then return finished. */
  if (byte < 0) {
    return GTEXT_DEC_FINISHED;
  }
  /* If Shift_JIS leading is not 0x00: */
  if (s->leading != 0) {
    /* Let leading be Shift_JIS leading. Set Shift_JIS leading to 0x00. Let pointer be null. */
    const uint32_t leading = s->leading;
    s->leading = 0;
    int64_t pointer = -1;
    /* Let offset be 0x40 if byte is less than 0x7F; otherwise 0x41. Let leadingOffset be 0x81 if leading is less than 0xA0; otherwise 0xC1. */
    const int offset = byte < 0x7F ? 0x40 : 0x41;
    const uint32_t leading_offset = leading < 0xA0 ? 0x81 : 0xC1;
    /* If byte is in the range 0x40 to 0x7E, inclusive, or 0x80 to 0xFC, inclusive, then set pointer to (leading - leadingOffset) x 188 + byte - offset. */
    if (in_range(byte, 0x40, 0x7E) || in_range(byte, 0x80, 0xFC)) {
      pointer = (int64_t)(leading - leading_offset) * 188 + byte - offset;
    }
    /* If pointer is in the range 8836 to 10715, inclusive, then return a code point whose value is 0xE000 - 8836 + pointer. */
    if (pointer >= 8836 && pointer <= 10715) {
      emit1(out, (uint32_t)(0xE000 - 8836 + pointer));
      return GTEXT_DEC_EMIT;
    }
    /* Let codePoint be null if pointer is null; otherwise the index code point for pointer in index jis0208. */
    const int32_t cp = pointer < 0 ? -1 : jis0208_code_point((uint32_t)pointer);
    /* If codePoint is non-null, then return a code point whose value is codePoint. */
    if (cp >= 0) {
      emit1(out, (uint32_t)cp);
      return GTEXT_DEC_EMIT;
    }
    /* If byte is an ASCII byte, then restore byte to ioQueue. */
    if (byte <= 0x7F) {
      out->restore_n = 1;
      out->restore[0] = (uint8_t)byte;
    }
    /* Return error. */
    return GTEXT_DEC_ERROR;
  }
  /* If byte is an ASCII byte or 0x80, then return a code point whose value is byte. */
  if (byte <= 0x80) {
    emit1(out, (uint32_t)byte);
    return GTEXT_DEC_EMIT;
  }
  /* If byte is in the range 0xA1 to 0xDF, inclusive, then return a code point whose value is 0xFF61 - 0xA1 + byte. */
  if (in_range(byte, 0xA1, 0xDF)) {
    emit1(out, 0xFF61u - 0xA1u + (uint32_t)byte);
    return GTEXT_DEC_EMIT;
  }
  /* If byte is in the range 0x81 to 0x9F, inclusive, or 0xE0 to 0xFC, inclusive, then set Shift_JIS leading to byte and return continue. */
  if (in_range(byte, 0x81, 0x9F) || in_range(byte, 0xE0, 0xFC)) {
    s->leading = (uint8_t)byte;
    return GTEXT_DEC_CONTINUE;
  }
  /* Return error. */
  return GTEXT_DEC_ERROR;
}

/// Shift_JIS's encoder's handler (12.3.2).
static GTEXT_Enc_Result shift_jis_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  (void)state;
  /* If codePoint is end-of-queue, then return finished. */
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  /* If codePoint is an ASCII code point or U+0080, then return a byte whose value is codePoint. */
  if (cp <= 0x80) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is U+00A5, then return byte 0x5C. If codePoint is U+203E, then return byte 0x7E. */
  if (cp == 0x00A5 || cp == 0x203E) {
    out->bytes[0] = cp == 0x00A5 ? 0x5C : 0x7E;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is in the range U+FF61 to U+FF9F, inclusive, then return a byte whose value is codePoint - 0xFF61 + 0xA1. */
  if (cp >= 0xFF61 && cp <= 0xFF9F) {
    out->bytes[0] = (uint8_t)(cp - 0xFF61 + 0xA1);
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* If codePoint is U+2212, then set it to U+FF0D. */
  if (cp == 0x2212) {
    cp = 0xFF0D;
  }
  /* Let pointer be the index Shift_JIS pointer for codePoint. If pointer is null, then return error with codePoint. */
  const uint32_t pointer = gtext_rev_lookup(&gtext_encoding_shift_jis_rev, (uint32_t)cp);
  if (pointer == GTEXT_INDEX_NONE) {
    out->error_cp = (uint32_t)cp;
    return GTEXT_ENC_ERROR;
  }
  /* Let leading be pointer / 188. Let leadingOffset be 0x81 if leading is less than 0x1F; otherwise 0xC1.
     Let trailing be pointer % 188. Let offset be 0x40 if trailing is less than 0x3F; otherwise 0x41. */
  const uint32_t leading = pointer / 188;
  const uint32_t leading_offset = leading < 0x1F ? 0x81 : 0xC1;
  const uint32_t trailing = pointer % 188;
  const uint32_t offset = trailing < 0x3F ? 0x40 : 0x41;
  out->bytes[0] = (uint8_t)(leading + leading_offset);
  out->bytes[1] = (uint8_t)(trailing + offset);
  out->n = 2;
  return GTEXT_ENC_OK;
}

/* ---- EUC-KR (section 13) ------------------------------------------------------------------------------ */

/// EUC-KR's decoder's handler (13.1.1).
static GTEXT_Dec_Result euc_kr_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  GTEXT_Lead_State * s = &state->lead;
  /* If byte is end-of-queue and EUC-KR leading is not 0x00, then set EUC-KR leading to 0x00 and return error. */
  if (byte < 0 && s->leading != 0) {
    s->leading = 0;
    return GTEXT_DEC_ERROR;
  }
  /* If byte is end-of-queue and EUC-KR leading is 0x00, then return finished. */
  if (byte < 0) {
    return GTEXT_DEC_FINISHED;
  }
  /* If EUC-KR leading is not 0x00: */
  if (s->leading != 0) {
    /* Let leading be EUC-KR leading. Set EUC-KR leading to 0x00. Let pointer be null. */
    const uint32_t leading = s->leading;
    s->leading = 0;
    int64_t pointer = -1;
    /* If byte is in the range 0x41 to 0xFE, inclusive, then set pointer to (leading - 0x81) x 190 + (byte - 0x41). */
    if (in_range(byte, 0x41, 0xFE)) {
      pointer = (int64_t)(leading - 0x81) * 190 + (byte - 0x41);
    }
    /* Let codePoint be null if pointer is null; otherwise the index code point for pointer in index EUC-KR. */
    if (pointer >= 0 && pointer < GTEXT_EUC_KR_POINTERS) {
      const uint16_t cp = gtext_encoding_euc_kr_decode[pointer];
      /* If codePoint is non-null, then return a code point whose value is codePoint. */
      if (cp != GTEXT_INDEX_NONE) {
        emit1(out, cp);
        return GTEXT_DEC_EMIT;
      }
    }
    /* If byte is an ASCII byte, then restore byte to ioQueue. */
    if (byte <= 0x7F) {
      out->restore_n = 1;
      out->restore[0] = (uint8_t)byte;
    }
    /* Return error. */
    return GTEXT_DEC_ERROR;
  }
  /* If byte is an ASCII byte, then return a code point whose value is byte. */
  if (byte <= 0x7F) {
    emit1(out, (uint32_t)byte);
    return GTEXT_DEC_EMIT;
  }
  /* If byte is in the range 0x81 to 0xFE, inclusive, then set EUC-KR leading to byte and return continue. */
  if (in_range(byte, 0x81, 0xFE)) {
    s->leading = (uint8_t)byte;
    return GTEXT_DEC_CONTINUE;
  }
  /* Return error. */
  return GTEXT_DEC_ERROR;
}

/// EUC-KR's encoder's handler (13.1.2).
static GTEXT_Enc_Result euc_kr_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  (void)state;
  /* If codePoint is end-of-queue, then return finished. */
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  /* If codePoint is an ASCII code point, then return a byte whose value is codePoint. */
  if (cp <= 0x7F) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  /* Let pointer be the index pointer for codePoint in index EUC-KR. If pointer is null, then return error with codePoint. */
  const uint32_t pointer = gtext_rev_lookup(&gtext_encoding_euc_kr_rev, (uint32_t)cp);
  if (pointer == GTEXT_INDEX_NONE) {
    return GTEXT_ENC_ERROR;
  }
  /* Let leading be pointer / 190 + 0x81. Let trailing be pointer % 190 + 0x41. Return two bytes. */
  out->bytes[0] = (uint8_t)(pointer / 190 + 0x81);
  out->bytes[1] = (uint8_t)(pointer % 190 + 0x41);
  out->n = 2;
  return GTEXT_ENC_OK;
}

/* ---- the codecs ------------------------------------------------------------------------------------------ */

static const GTEXT_Dec_Codec dec_gb18030 = {init_zero, gb18030_step, gb18030_idle};
static const GTEXT_Dec_Codec dec_big5 = {init_zero, big5_step, lead_idle};
static const GTEXT_Dec_Codec dec_euc_jp = {init_zero, euc_jp_step, lead_idle};
/* ISO-2022-JP: 0x0E, 0x0F and 0x1B are not their own code points, so no ASCII run may be copied whole. */
static const GTEXT_Dec_Codec dec_iso_2022_jp = {iso_2022_jp_dec_init, iso_2022_jp_step, NULL};
static const GTEXT_Dec_Codec dec_shift_jis = {init_zero, shift_jis_step, lead_idle};
static const GTEXT_Dec_Codec dec_euc_kr = {init_zero, euc_kr_step, lead_idle};

static void enc_init_zero(GTEXT_Enc_State * state, GTEXT_Encoding encoding) {
  (void)encoding;
  memset(state, 0, sizeof(*state));
}

static const GTEXT_Enc_Codec enc_gb18030 = {enc_init_zero, gb18030_enc_step, true};
static const GTEXT_Enc_Codec enc_gbk = {enc_init_zero, gbk_enc_step, true};
static const GTEXT_Enc_Codec enc_big5 = {enc_init_zero, big5_enc_step, true};
static const GTEXT_Enc_Codec enc_euc_jp = {enc_init_zero, euc_jp_enc_step, true};
static const GTEXT_Enc_Codec enc_iso_2022_jp = {iso_2022_jp_enc_init, iso_2022_jp_enc_step, false};
static const GTEXT_Enc_Codec enc_shift_jis = {enc_init_zero, shift_jis_enc_step, true};
static const GTEXT_Enc_Codec enc_euc_kr = {enc_init_zero, euc_kr_enc_step, true};

/** The decoder row of a legacy multi-byte encoding, or NULL for any other. */
const GTEXT_Dec_Codec * gtext_encoding_cjk_dec_codec(GTEXT_Encoding encoding) {
  switch (encoding) {
  case GTEXT_ENCODING_GBK:
  case GTEXT_ENCODING_GB18030:
    return &dec_gb18030;
  case GTEXT_ENCODING_BIG5:
    return &dec_big5;
  case GTEXT_ENCODING_EUC_JP:
    return &dec_euc_jp;
  case GTEXT_ENCODING_ISO_2022_JP:
    return &dec_iso_2022_jp;
  case GTEXT_ENCODING_SHIFT_JIS:
    return &dec_shift_jis;
  case GTEXT_ENCODING_EUC_KR:
    return &dec_euc_kr;
  default:
    return NULL;
  }
}

/** The encoder row of a legacy multi-byte encoding, or NULL for any other. */
const GTEXT_Enc_Codec * gtext_encoding_cjk_enc_codec(GTEXT_Encoding encoding) {
  switch (encoding) {
  case GTEXT_ENCODING_GBK:
    return &enc_gbk;
  case GTEXT_ENCODING_GB18030:
    return &enc_gb18030;
  case GTEXT_ENCODING_BIG5:
    return &enc_big5;
  case GTEXT_ENCODING_EUC_JP:
    return &enc_euc_jp;
  case GTEXT_ENCODING_ISO_2022_JP:
    return &enc_iso_2022_jp;
  case GTEXT_ENCODING_SHIFT_JIS:
    return &enc_shift_jis;
  case GTEXT_ENCODING_EUC_KR:
    return &enc_euc_kr;
  default:
    return NULL;
  }
}
