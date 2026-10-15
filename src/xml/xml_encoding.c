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

/* The converter is incremental: a document may arrive in pieces that cut a
   UTF-16 or UTF-32 unit, or a surrogate pair, anywhere. What carries across a
   piece is a partial unit (`left`), a high surrogate waiting for its low one
   (`high`), and the line, column and byte count that a position in an error is
   made of. The whole-document parse feeds one piece and ends it, so there is one
   conversion and not two. */

void xml_converter_init(
    xml_converter * c, xml_ctx * p, markup_buf * out, xml_family family) {
  memset(c, 0, sizeof(*c));
  c->p = p;
  c->out = out;
  c->family = family;
  c->line = 1;
  c->column = 1;
}

static int conv_select_profile(xml_converter * c);

static GTEXT_XML_Status conv_invalid(xml_converter * c, const char * message,
    size_t raw_offset) {
  conv_select_profile(c);
  GTEXT_Markup_Location where;
  memset(&where, 0, sizeof(where));
  where.offset = c->total;
  where.line = c->line;
  where.column = c->column;
  /* Conversion can run for a resolver-owned physical source. */
  where.source_id = c->p->source_count
      ? c->p->sources[c->p->source_count - 1].source_id : 1;
  where.raw_offset = raw_offset;
  where.has_raw_offset = 1;
  return xml_fail_at(c->p, GTEXT_XML_E_INVALID_SEQUENCE, message, &where);
}

/* The version is the first pseudo-attribute in an XML declaration. The
   converter needs it only to locate a later decoding error correctly; the XML
   parser remains the authority that validates the complete declaration. */
static int conv_select_profile(xml_converter * c) {
  if (c->version == XML_VERSION_11) {
    return 0;
  }
  xml_ctx probe;
  memset(&probe, 0, sizeof(probe));
  probe.in = (const unsigned char *)c->out->data;
  probe.length = c->out->length;
  xml_probe_xml11_declaration(&probe);
  if (probe.version != XML_VERSION_11) {
    return 0;
  }
  c->version = probe.version;
  c->line = 1;
  c->column = 1;
  c->previous = 0;
  for (size_t i = 0; i < c->out->length;) {
    const unsigned char b = (unsigned char)c->out->data[i];
    uint32_t cp = b;
    size_t width = 1;
    if (b >= 0x80) {
      const size_t left = c->out->length - i;
      if (b >= 0xC2 && b <= 0xDF && left >= 2) {
        cp = ((uint32_t)(b & 0x1F) << 6)
            | ((unsigned char)c->out->data[i + 1] & 0x3F);
        width = 2;
      }
      else if (b >= 0xE0 && b <= 0xEF && left >= 3) {
        cp = ((uint32_t)(b & 0x0F) << 12)
            | (((unsigned char)c->out->data[i + 1] & 0x3F) << 6)
            | ((unsigned char)c->out->data[i + 2] & 0x3F);
        width = 3;
      }
      else if (b >= 0xF0 && b <= 0xF4 && left >= 4) {
        cp = ((uint32_t)(b & 0x07) << 18)
            | (((unsigned char)c->out->data[i + 1] & 0x3F) << 12)
            | (((unsigned char)c->out->data[i + 2] & 0x3F) << 6)
            | ((unsigned char)c->out->data[i + 3] & 0x3F);
        width = 4;
      }
    }
    if (cp == '\r') {
      c->line++;
      c->column = 1;
    }
    else if (cp == '\n' || cp == 0x85 || cp == 0x2028) {
      if (c->previous != '\r') {
        c->line++;
      }
      c->column = 1;
    }
    else {
      c->column++;
    }
    c->previous = cp;
    i += width;
  }
  return 1;
}

/* Append one character, keeping the line and column of the original. */
static GTEXT_XML_Status conv_put(xml_converter * c, uint32_t cp) {
  const size_t before = c->out->length;
  GTEXT_XML_Status status = xml_append_code_point(c->p, c->out, cp);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  const int selected = conv_select_profile(c);
  c->total += c->out->length - before;
  if (selected) {
    return GTEXT_XML_OK;
  }
  if (cp == '\n') {
    if (c->previous != '\r') {
      c->line++;
    }
    c->column = 1;
  }
  else if (c->version == XML_VERSION_11 && (cp == 0x85 || cp == 0x2028)) {
    if (c->previous != '\r') {
      c->line++;
    }
    c->column = 1;
  }
  else if (cp == '\r') {
    c->line++;
    c->column = 1;
  }
  else {
    c->column++;
  }
  c->previous = cp;
  return GTEXT_XML_OK;
}

/* The next whole code unit of `width` bytes, from the bytes left over from the
   last piece and then from this one. 0 if there is not a whole one yet; what
   there is stays in `left`. */
static int take_unit(xml_converter * c, const unsigned char ** data, size_t * n,
    size_t width, int big, uint32_t * unit) {
  const unsigned char * u;
  if (c->left_n == 0 && *n >= width) {
    u = *data;
    *data += width;
    *n -= width;
  }
  else {
    while (c->left_n < width && *n > 0) {
      c->left[c->left_n++] = **data;
      (*data)++;
      (*n)--;
    }
    if (c->left_n < width) {
      return 0;
    }
    u = c->left;
    c->left_n = 0;
  }
  if (width == 2) {
    *unit = big ? (uint32_t)(u[0] << 8 | u[1]) : (uint32_t)(u[1] << 8 | u[0]);
  }
  else {
    *unit = big ? ((uint32_t)u[0] << 24 | (uint32_t)u[1] << 16
                      | (uint32_t)u[2] << 8 | u[3])
                : ((uint32_t)u[3] << 24 | (uint32_t)u[2] << 16
                      | (uint32_t)u[1] << 8 | u[0]);
  }
  return 1;
}

GTEXT_XML_Status xml_converter_feed(xml_converter * c,
    const unsigned char * data, size_t n, int final) {
  GTEXT_XML_Status status = GTEXT_XML_OK;
  const xml_family family = c->family;
  const unsigned char * const chunk = data;
  const size_t chunk_offset = c->raw_total;
  c->raw_total = n > SIZE_MAX - c->raw_total ? SIZE_MAX : c->raw_total + n;

  if (family == XML_FAMILY_LATIN1 || family == XML_FAMILY_ASCII) {
    for (size_t i = 0; i < n && status == GTEXT_XML_OK; i++) {
      if (data[i] >= 0x80 && family == XML_FAMILY_ASCII) {
        return conv_invalid(c, "a byte above 0x7F in a US-ASCII document",
            chunk_offset + i);
      }
      status = conv_put(c, data[i]);
    }
    return status;
  }
  if (family == XML_FAMILY_UTF16LE || family == XML_FAMILY_UTF16BE) {
    const int big = family == XML_FAMILY_UTF16BE;
    uint32_t u;
    for (;;) {
      const size_t left_before = c->left_n;
      const unsigned char * const before = data;
      if (!take_unit(c, &data, &n, 2, big, &u)) break;
      c->raw_unit_offset = left_before != 0
          ? chunk_offset - left_before
          : chunk_offset + (size_t)(before - chunk);
      if (c->high != 0) {
        if (u < 0xDC00 || u > 0xDFFF) {
          return conv_invalid(c, "a high surrogate not followed by a low one",
              c->high_raw_offset);
        }
        const uint32_t cp = 0x10000 + ((c->high - 0xD800) << 10) + (u - 0xDC00);
        c->high = 0;
        status = conv_put(c, cp);
      }
      else if (u >= 0xDC00 && u <= 0xDFFF) {
        return conv_invalid(c, "a low surrogate with no high surrogate before it",
            c->raw_unit_offset);
      }
      else if (u >= 0xD800 && u <= 0xDBFF) {
        c->high = u;
        c->high_raw_offset = c->raw_unit_offset;
        continue;
      }
      else {
        status = conv_put(c, u);
      }
      if (status != GTEXT_XML_OK) {
        return status;
      }
    }
    if (final) {
      if (c->high != 0 && c->left_n == 0) {
        return conv_invalid(c, "a high surrogate at the end of the document",
            c->high_raw_offset);
      }
      if (c->left_n != 0 || c->high != 0) {
        return conv_invalid(c, "a UTF-16 document ends inside a code unit",
            c->raw_total - c->left_n);
      }
    }
    return GTEXT_XML_OK;
  }
  { /* UTF-32 */
    const int big = family == XML_FAMILY_UTF32BE;
    uint32_t u;
    for (;;) {
      const size_t left_before = c->left_n;
      const unsigned char * const before = data;
      if (!take_unit(c, &data, &n, 4, big, &u)) break;
      c->raw_unit_offset = left_before != 0
          ? chunk_offset - left_before
          : chunk_offset + (size_t)(before - chunk);
      if (u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) {
        return conv_invalid(c, "a UTF-32 unit that is not a Unicode scalar value",
            c->raw_unit_offset);
      }
      status = conv_put(c, u);
      if (status != GTEXT_XML_OK) {
        return status;
      }
    }
    if (final && c->left_n != 0) {
      return conv_invalid(c, "a UTF-32 document ends inside a code unit",
          c->raw_total - c->left_n);
    }
    return GTEXT_XML_OK;
  }
}

static GTEXT_XML_Status convert(xml_ctx * p, xml_family family, size_t skip) {
  xml_converter c;
  xml_converter_init(&c, p, &p->decoded, family);
  c.raw_total = skip;
  const GTEXT_XML_Status status =
      xml_converter_feed(&c, p->in + skip, p->length - skip, 1);
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

int xml_sniff(const unsigned char * s, size_t n, xml_family * out_family,
    size_t * out_skip, int * out_bom) {
  xml_family family = XML_FAMILY_UTF8;
  size_t skip = 0;
  int bom = 0;

  if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
    *out_family = XML_FAMILY_UTF8;
    *out_skip = 3; /* A mark is not a character of the document: column 1 still. */
    *out_bom = 1;
    return 0;
  }
  if (n >= 4 && s[0] == 0xFF && s[1] == 0xFE && s[2] == 0 && s[3] == 0) {
    family = XML_FAMILY_UTF32LE;
    skip = 4;
    bom = 1;
  }
  else if (n >= 4 && s[0] == 0 && s[1] == 0 && s[2] == 0xFE && s[3] == 0xFF) {
    family = XML_FAMILY_UTF32BE;
    skip = 4;
    bom = 1;
  }
  else if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) {
    family = XML_FAMILY_UTF16LE;
    skip = 2;
    bom = 1;
  }
  else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) {
    family = XML_FAMILY_UTF16BE;
    skip = 2;
    bom = 1;
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
    return 1;
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
  }
  *out_family = family;
  *out_skip = skip;
  *out_bom = bom;
  return 0;
}

int xml_sniff_needs_more(const unsigned char * s, size_t n, int * has_close) {
  if (n < 4) {
    return 1;
  }
  xml_family family;
  size_t skip;
  int bom;
  if (xml_sniff(s, n, &family, &skip, &bom) != 0 || bom
      || (family != XML_FAMILY_UTF8 && family != XML_FAMILY_LATIN1
          && family != XML_FAMILY_ASCII)) {
    return 0; /* Decided by the first bytes. */
  }
  /* ASCII-compatible with no mark: a declaration may still say Latin-1 or
     ASCII, and it is only read once it has ended. */
  const size_t m = n < 5 ? n : 5;
  if (memcmp(s, "<?xml", m) != 0) {
    return 0;
  }
  if (n < 6) {
    return 1;
  }
  if (!xml_is_space(s[5])) {
    return 0;
  }
  return !*has_close;
}

GTEXT_XML_Status xml_detect_encoding(xml_ctx * p) {
  p->enc_family = XML_FAMILY_UTF8;
  p->enc_bom = 0;
  xml_family family;
  size_t skip;
  int bom;
  if (xml_sniff(p->in, p->length, &family, &skip, &bom)) {
    return xml_fail(p, GTEXT_XML_E_UNSUPPORTED_ENCODING,
        "EBCDIC is not supported");
  }
  p->enc_bom = bom;
  if (family == XML_FAMILY_UTF8) {
    p->pos = skip;
    return GTEXT_XML_OK;
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
