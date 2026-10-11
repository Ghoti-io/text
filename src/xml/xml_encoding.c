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
 * Which encoding a document is in, and turning it into UTF-8.
 *
 * XML 1.0 (Fifth Edition) Appendix F: the first bytes (a byte order mark, or
 * the bytes of "<?xml" in the encoding's width) say which family the document
 * is in before any of it can be read. The parser itself reads only UTF-8, so a
 * UTF-16 or UTF-32 document, and a document that declares ISO-8859-1 or
 * US-ASCII, is converted to UTF-8 first, in one pass, into a buffer from the
 * document's allocator. The parser is not told anything else: it checks the
 * declaration's label against `ctx->enc_family` and `ctx->enc_bom` (see
 * xml_check_label()).
 *
 * Offsets in an error or a node for converted input count bytes of the UTF-8
 * form; lines and columns count characters of the original.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "xml_internal.h"

/* ---- the label, in raw bytes -------------------------------------------- */

static int ci_equals(const unsigned char * s, size_t n, const char * t) {
  if (n != strlen(t)) {
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char a = s[i];
    unsigned char b = (unsigned char)t[i];
    if (a >= 'A' && a <= 'Z') {
      a = (unsigned char)(a + 32);
    }
    if (b >= 'A' && b <= 'Z') {
      b = (unsigned char)(b + 32);
    }
    if (a != b) {
      return 0;
    }
  }
  return 1;
}

xml_label xml_classify_label(const unsigned char * s, size_t n) {
  if (ci_equals(s, n, "UTF-8")) {
    return XML_LABEL_UTF8;
  }
  if (ci_equals(s, n, "UTF-16")) {
    return XML_LABEL_UTF16;
  }
  if (ci_equals(s, n, "UTF-32")) {
    return XML_LABEL_UTF32;
  }
  if (ci_equals(s, n, "ISO-8859-1")) {
    return XML_LABEL_LATIN1;
  }
  if (ci_equals(s, n, "US-ASCII")) {
    return XML_LABEL_ASCII;
  }
  return XML_LABEL_OTHER;
}

static size_t raw_space(const unsigned char * s, size_t n, size_t i) {
  while (i < n && xml_is_space(s[i])) {
    i++;
  }
  return i;
}

/* One `name = "value"` of the declaration at `*i`. Returns 0 if there is not
   one; otherwise sets the name and value ranges and moves `*i` past the value. */
static int raw_pseudo(const unsigned char * s, size_t n, size_t * i,
    size_t * name_at, size_t * name_len, size_t * value_at, size_t * value_len) {
  size_t k = *i;
  *name_at = k;
  while (k < n && ((s[k] | 0x20) >= 'a' && (s[k] | 0x20) <= 'z')) {
    k++;
  }
  *name_len = k - *name_at;
  if (*name_len == 0) {
    return 0;
  }
  k = raw_space(s, n, k);
  if (k >= n || s[k] != '=') {
    return 0;
  }
  k = raw_space(s, n, k + 1);
  if (k >= n || (s[k] != '"' && s[k] != '\'')) {
    return 0;
  }
  const unsigned char quote = s[k++];
  *value_at = k;
  while (k < n && s[k] != quote && s[k] != '<' && s[k] != '>') {
    k++;
  }
  if (k >= n || s[k] != quote) {
    return 0;
  }
  *value_len = k - *value_at;
  *i = k + 1;
  return 1;
}

/* The encoding label of an XML declaration at the start of `s`, if there is
   one that is shaped like the production. */
static int raw_label(const unsigned char * s, size_t n, size_t * at, size_t * len) {
  if (n < 6 || memcmp(s, "<?xml", 5) != 0 || !xml_is_space(s[5])) {
    return 0;
  }
  size_t i = raw_space(s, n, 5);
  for (int guard = 0; guard < 3; guard++) {
    size_t name_at, name_len, value_at, value_len;
    if (!raw_pseudo(s, n, &i, &name_at, &name_len, &value_at, &value_len)) {
      return 0;
    }
    if (name_len == 8 && memcmp(s + name_at, "encoding", 8) == 0) {
      *at = value_at;
      *len = value_len;
      return 1;
    }
    i = raw_space(s, n, i);
  }
  return 0;
}

/* ---- conversion --------------------------------------------------------- */

typedef struct {
  xml_ctx * p;
  markup_buf * out;
  size_t line;
  size_t column;
  uint32_t previous;
} xml_sink;

static GTEXT_XML_Status sink_invalid(xml_sink * s, const char * message) {
  GTEXT_Markup_Location where;
  where.offset = s->out->length;
  where.line = s->line;
  where.column = s->column;
  return xml_fail_at(s->p, GTEXT_XML_E_INVALID_SEQUENCE, message, &where);
}

/* Append one character, keeping the line and column of the original. */
static GTEXT_XML_Status sink_put(xml_sink * s, uint32_t cp) {
  GTEXT_XML_Status status = xml_append_code_point(s->p, s->out, cp);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  if (cp == '\n') {
    if (s->previous != '\r') {
      s->line++;
    }
    s->column = 1;
  }
  else if (cp == '\r') {
    s->line++;
    s->column = 1;
  }
  else {
    s->column++;
  }
  s->previous = cp;
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status convert(xml_ctx * p, xml_family family, size_t skip) {
  const unsigned char * in = p->in;
  const size_t n = p->length;
  xml_sink sink = {p, &p->decoded, 1, 1, 0};
  GTEXT_XML_Status status = GTEXT_XML_OK;

  if (family == XML_FAMILY_LATIN1 || family == XML_FAMILY_ASCII) {
    for (size_t i = 0; i < n && status == GTEXT_XML_OK; i++) {
      if (in[i] >= 0x80 && family == XML_FAMILY_ASCII) {
        return sink_invalid(&sink, "a byte above 0x7F in a US-ASCII document");
      }
      status = sink_put(&sink, in[i]);
    }
  }
  else if (family == XML_FAMILY_UTF16LE || family == XML_FAMILY_UTF16BE) {
    const int big = family == XML_FAMILY_UTF16BE;
    size_t i = skip;
    while (i < n && status == GTEXT_XML_OK) {
      if (n - i < 2) {
        return sink_invalid(&sink, "a UTF-16 document ends inside a code unit");
      }
      uint32_t u = big ? (uint32_t)(in[i] << 8 | in[i + 1])
                       : (uint32_t)(in[i + 1] << 8 | in[i]);
      i += 2;
      if (u >= 0xDC00 && u <= 0xDFFF) {
        return sink_invalid(&sink, "a low surrogate with no high surrogate before it");
      }
      if (u >= 0xD800 && u <= 0xDBFF) {
        if (n - i < 2) {
          return sink_invalid(&sink,
              n - i == 0 ? "a high surrogate at the end of the document"
                         : "a UTF-16 document ends inside a code unit");
        }
        const uint32_t v = big ? (uint32_t)(in[i] << 8 | in[i + 1])
                               : (uint32_t)(in[i + 1] << 8 | in[i]);
        if (v < 0xDC00 || v > 0xDFFF) {
          return sink_invalid(&sink, "a high surrogate not followed by a low one");
        }
        i += 2;
        u = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00);
      }
      status = sink_put(&sink, u);
    }
  }
  else { /* UTF-32 */
    const int big = family == XML_FAMILY_UTF32BE;
    size_t i = skip;
    while (i < n && status == GTEXT_XML_OK) {
      if (n - i < 4) {
        return sink_invalid(&sink, "a UTF-32 document ends inside a code unit");
      }
      const uint32_t u = big
          ? ((uint32_t)in[i] << 24 | (uint32_t)in[i + 1] << 16
                | (uint32_t)in[i + 2] << 8 | in[i + 3])
          : ((uint32_t)in[i + 3] << 24 | (uint32_t)in[i + 2] << 16
                | (uint32_t)in[i + 1] << 8 | in[i]);
      i += 4;
      if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) {
        return sink_invalid(&sink, "a UTF-32 unit that is not a Unicode scalar value");
      }
      status = sink_put(&sink, u);
    }
  }
  if (status != GTEXT_XML_OK) {
    return status;
  }
  /* The parser reads the converted text from here on. */
  static const unsigned char empty[1] = {0};
  p->in = p->decoded.data ? (const unsigned char *)p->decoded.data : empty;
  p->length = p->decoded.length;
  p->pos = 0;
  return GTEXT_XML_OK;
}

/* ---- detection ---------------------------------------------------------- */

GTEXT_XML_Status xml_detect_encoding(xml_ctx * p) {
  const unsigned char * s = p->in;
  const size_t n = p->length;
  p->enc_family = XML_FAMILY_UTF8;
  p->enc_bom = 0;

  if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
    p->enc_bom = 1;
    p->pos = 3; /* A mark is not a character of the document: column 1 still. */
    return GTEXT_XML_OK;
  }
  xml_family family = XML_FAMILY_UTF8;
  size_t skip = 0;
  if (n >= 4 && s[0] == 0xFF && s[1] == 0xFE && s[2] == 0 && s[3] == 0) {
    family = XML_FAMILY_UTF32LE;
    skip = 4;
    p->enc_bom = 1;
  }
  else if (n >= 4 && s[0] == 0 && s[1] == 0 && s[2] == 0xFE && s[3] == 0xFF) {
    family = XML_FAMILY_UTF32BE;
    skip = 4;
    p->enc_bom = 1;
  }
  else if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) {
    family = XML_FAMILY_UTF16LE;
    skip = 2;
    p->enc_bom = 1;
  }
  else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
    family = XML_FAMILY_UTF16BE;
    skip = 2;
    p->enc_bom = 1;
  }
  else if (n >= 4 && s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 0x3C) {
    family = XML_FAMILY_UTF32BE;
  }
  else if (n >= 4 && s[0] == 0x3C && s[1] == 0 && s[2] == 0 && s[3] == 0) {
    family = XML_FAMILY_UTF32LE;
  }
  else if (n >= 4 && s[0] == 0 && s[1] == 0x3C && s[2] == 0 && s[3] == 0x3F) {
    family = XML_FAMILY_UTF16BE;
  }
  else if (n >= 4 && s[0] == 0x3C && s[1] == 0 && s[2] == 0x3F && s[3] == 0) {
    family = XML_FAMILY_UTF16LE;
  }
  else if (n >= 4 && s[0] == 0x4C && s[1] == 0x6F && s[2] == 0xA7 && s[3] == 0x94) {
    return xml_fail(p, GTEXT_XML_E_UNSUPPORTED_ENCODING,
        "EBCDIC is not supported");
  }

  if (family == XML_FAMILY_UTF8) {
    /* ASCII-compatible, no mark: the declaration (in ASCII, whatever follows)
       may name one of the two single-byte encodings. */
    size_t at = 0;
    size_t len = 0;
    if (raw_label(s, n, &at, &len)) {
      const xml_label label = xml_classify_label(s + at, len);
      if (label == XML_LABEL_LATIN1 || label == XML_LABEL_ASCII) {
        family = label == XML_LABEL_LATIN1 ? XML_FAMILY_LATIN1 : XML_FAMILY_ASCII;
      }
    }
    if (family == XML_FAMILY_UTF8) {
      return GTEXT_XML_OK;
    }
  }
  p->enc_family = family;
  return convert(p, family, skip);
}

/* ---- the label against the family --------------------------------------- */

GTEXT_XML_Status xml_check_label(xml_ctx * p, const unsigned char * label,
    size_t length, const GTEXT_Markup_Location * where) {
  const xml_label named = xml_classify_label(label, length);
  xml_label expected;
  switch (p->enc_family) {
    case XML_FAMILY_UTF16LE:
    case XML_FAMILY_UTF16BE:
      expected = XML_LABEL_UTF16;
      break;
    case XML_FAMILY_UTF32LE:
    case XML_FAMILY_UTF32BE:
      expected = XML_LABEL_UTF32;
      break;
    case XML_FAMILY_LATIN1:
      expected = XML_LABEL_LATIN1;
      break;
    case XML_FAMILY_ASCII:
      expected = XML_LABEL_ASCII;
      break;
    default:
      expected = XML_LABEL_UTF8;
      break;
  }
  if (named == expected) {
    return GTEXT_XML_OK;
  }
  if (named == XML_LABEL_OTHER) {
    return xml_fail_at(p, GTEXT_XML_E_UNSUPPORTED_ENCODING,
        "only UTF-8, UTF-16, UTF-32, ISO-8859-1 and US-ASCII are supported", where);
  }
  return xml_fail_at(p, GTEXT_XML_E_ENCODING_MISMATCH,
      "the encoding named in the declaration is not the one the document is in",
      where);
}

const char * xml_family_name(xml_family family) {
  switch (family) {
    case XML_FAMILY_UTF16LE:
    case XML_FAMILY_UTF16BE:
      return "UTF-16";
    case XML_FAMILY_UTF32LE:
    case XML_FAMILY_UTF32BE:
      return "UTF-32";
    case XML_FAMILY_LATIN1:
      return "ISO-8859-1";
    case XML_FAMILY_ASCII:
      return "US-ASCII";
    default:
      return "UTF-8";
  }
}
