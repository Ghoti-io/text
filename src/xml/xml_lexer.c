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
 * The XML parser's cursor and lexical pieces: characters, names, references,
 * runs of character data, and the small set the attribute checks use.
 *
 * XML 1.0 (Fifth Edition) [2] Char, [3] S, [4]-[5] Name, [66] CharRef,
 * [67]-[68] Reference, [14] CharData.
 */

#include <ghoti.io/text/macros.h>

#include <stdlib.h>
#include <string.h>

#include "xml_internal.h"

/* ---- the cursor --------------------------------------------------------- */

void xml_consume(xml_ctx * p, size_t n) {
  const unsigned char * in = p->in;
  const size_t end = p->pos + n;
  for (size_t i = p->pos; i < end;) {
    const unsigned char b = in[i];
    uint32_t cp = b;
    size_t width = 1;
    if (b >= 0x80) {
      const xml_char_status decoded = xml_decode(p, i, &cp, &width);
      if (decoded == XML_CHAR_BAD_UTF8 || decoded == XML_CHAR_EOF) {
        /* Invalid input is reported by the caller; preserve the old cursor
           convention while it advances to the offending byte. */
        cp = UINT32_MAX;
        width = 1;
        if ((b & 0xC0) == 0x80) {
          i++;
          continue;
        }
      }
    }
    if (cp == '\r') {
      p->line++;
      p->column = 1;
    }
    else if (cp == '\n' || (p->version == XML_VERSION_11
        && (cp == 0x85 || cp == 0x2028))) {
      /* LF and XML 1.1 NEL following CR are one logical line end. */
      if (p->cursor_previous != '\r') {
        p->line++;
      }
      p->column = 1;
    }
    else {
      p->column++;
    }
    p->cursor_previous = cp;
    i += width;
  }
  p->pos += n;
}

int xml_looking_at(const xml_ctx * p, const char * literal, size_t length) {
  return p->length - p->pos >= length
      && memcmp(p->in + p->pos, literal, length) == 0;
}

int xml_cut_literal(const xml_ctx * p, const char * literal, size_t length) {
  const size_t have = p->length - p->pos;
  return p->partial && have < length && memcmp(p->in + p->pos, literal, have) == 0;
}

int xml_at_end(const xml_ctx * p, size_t at) {
  if (at >= p->length) {
    return 1;
  }
  uint32_t cp = 0;
  size_t n = 0;
  return p->partial && xml_decode(p, at, &cp, &n) == XML_CHAR_EOF;
}

GTEXT_XML_Status xml_expect(
    xml_ctx * p, const char * literal, size_t length, const char * message) {
  if (!xml_looking_at(p, literal, length)) {
    if (xml_cut_literal(p, literal, length)) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    }
    return xml_unexpected(p, message);
  }
  xml_consume(p, length);
  return GTEXT_XML_OK;
}

size_t xml_skip_space(xml_ctx * p) {
  size_t i = p->pos;
  while (i < p->length) {
    const size_t width = xml_space_bytes(p, i);
    if (width == 0) {
      break;
    }
    i += width;
  }
  const size_t n = i - p->pos;
  if (n > 0) {
    xml_consume(p, n);
  }
  return n;
}

/* ---- characters --------------------------------------------------------- */

static int is_xml_char(uint32_t c) {
  return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF)
      || (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF);
}

static int is_xml11_char(uint32_t c) {
  return (c >= 0x1 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD)
      || (c >= 0x10000 && c <= 0x10FFFF);
}

static int is_xml11_restricted(uint32_t c) {
  return (c >= 0x1 && c <= 0x8) || c == 0xB || c == 0xC
      || (c >= 0xE && c <= 0x1F) || (c >= 0x7F && c <= 0x84)
      || (c >= 0x86 && c <= 0x9F);
}

xml_char_status xml_decode(
    const xml_ctx * p, size_t at, uint32_t * code_point, size_t * bytes) {
  if (at >= p->length) {
    return XML_CHAR_EOF;
  }
  const unsigned char * s = p->in + at;
  const size_t left = p->length - at;
  const unsigned char b0 = s[0];
  uint32_t cp;
  size_t n;
  if (b0 < 0x80) {
    cp = b0;
    n = 1;
  }
  else if (b0 >= 0xC2 && b0 <= 0xF4) {
    n = b0 <= 0xDF ? 2 : (b0 <= 0xEF ? 3 : 4);
    /* What is there must be the start of a character before it is a short one:
       a continuation byte that is not, or a second byte that makes it overlong,
       a surrogate or beyond U+10FFFF, is wrong whatever follows. */
    const size_t have = left < n ? left : n;
    for (size_t i = 1; i < have; i++) {
      if ((s[i] & 0xC0) != 0x80) {
        return XML_CHAR_BAD_UTF8;
      }
    }
    if (have >= 2
        && ((b0 == 0xE0 && s[1] < 0xA0) || (b0 == 0xED && s[1] > 0x9F)
            || (b0 == 0xF0 && s[1] < 0x90) || (b0 == 0xF4 && s[1] > 0x8F))) {
      return XML_CHAR_BAD_UTF8; /* overlong, surrogate, beyond U+10FFFF */
    }
    if (left < n) {
      /* A character cut short by the end of what has arrived is not yet wrong. */
      return p->partial ? XML_CHAR_EOF : XML_CHAR_BAD_UTF8;
    }
    if (n == 2) {
      cp = ((uint32_t)(b0 & 0x1F) << 6) | (s[1] & 0x3F);
    }
    else if (n == 3) {
      cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6)
          | (s[2] & 0x3F);
    }
    else {
      cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12)
          | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
  }
  else {
    return XML_CHAR_BAD_UTF8;
  }
  *code_point = cp;
  *bytes = n;
  if (p->version == XML_VERSION_11) {
    if (!is_xml11_char(cp)) {
      return XML_CHAR_FORBIDDEN;
    }
    return is_xml11_restricted(cp) ? XML_CHAR_RESTRICTED : XML_CHAR_OK;
  }
  return is_xml_char(cp) ? XML_CHAR_OK : XML_CHAR_FORBIDDEN;
}

/* ---- memory ------------------------------------------------------------- */

void * xml_alloc(xml_ctx * p, size_t size) {
  return gtext_allocator_calloc(p->allocator, 1, size);
}

void xml_release(xml_ctx * p, void * ptr) {
  gtext_allocator_free(p->allocator, ptr);
}

GTEXT_XML_Status xml_grow(xml_ctx * p, void ** array, size_t * capacity,
    size_t element_size, size_t needed) {
  if (needed <= *capacity) {
    return GTEXT_XML_OK;
  }
  size_t cap = *capacity ? *capacity : 8;
  while (cap < needed) {
    if (cap > SIZE_MAX / 2) {
      return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
    }
    cap *= 2;
  }
  if (cap > SIZE_MAX / element_size) {
    return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
  }
  void * grown = gtext_allocator_realloc(p->allocator, *array, cap * element_size);
  if (!grown) {
    return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
  }
  *array = grown;
  *capacity = cap;
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_append(
    xml_ctx * p, markup_buf * buf, const char * bytes, size_t length) {
  if (markup_buf_append(buf, bytes, length) != GTEXT_MARKUP_OK) {
    return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
  }
  return GTEXT_XML_OK;
}

GTEXT_XML_Status xml_append_code_point(
    xml_ctx * p, markup_buf * buf, uint32_t cp) {
  char out[4];
  size_t n;
  if (cp < 0x80) {
    out[0] = (char)cp;
    n = 1;
  }
  else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  }
  else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  }
  else {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  return xml_append(p, buf, out, n);
}

static size_t line_end_bytes_for_version(
    const xml_ctx * p, size_t at, int xml11) {
  if (at >= p->length) {
    return 0;
  }
  uint32_t cp = 0;
  size_t width = 0;
  const xml_char_status decoded = xml_decode(p, at, &cp, &width);
  if (decoded == XML_CHAR_BAD_UTF8 || decoded == XML_CHAR_EOF) {
    return 0;
  }
  if (cp == '\r') {
    const size_t next = at + width;
    if (next < p->length) {
      uint32_t following = 0;
      size_t following_width = 0;
      const xml_char_status following_status =
          xml_decode(p, next, &following, &following_width);
      if (following_status != XML_CHAR_BAD_UTF8
          && following_status != XML_CHAR_EOF
          && (following == '\n' || (xml11 && following == 0x85))) {
        return width + following_width;
      }
    }
    return width;
  }
  if (cp == '\n' || (xml11 && (cp == 0x85 || cp == 0x2028))) {
    return width;
  }
  return 0;
}

size_t xml_line_end_bytes(const xml_ctx * p, size_t at) {
  return line_end_bytes_for_version(p, at, p->version == XML_VERSION_11);
}

size_t xml_xml11_line_end_bytes(const xml_ctx * p, size_t at) {
  return line_end_bytes_for_version(p, at, 1);
}

size_t xml_space_bytes(const xml_ctx * p, size_t at) {
  if (at >= p->length) {
    return 0;
  }
  const unsigned char b = p->in[at];
  if (b == ' ' || b == '\t') {
    return 1;
  }
  return xml_line_end_bytes(p, at);
}

void xml_probe_xml11_declaration(xml_ctx * p) {
  if (p->version == XML_VERSION_11 || !xml_looking_at(p, "<?xml", 5)) {
    return;
  }
  size_t i = p->pos + 5;
  size_t sep = xml_xml11_line_end_bytes(p, i);
  if (sep == 0 && i < p->length
      && (p->in[i] == ' ' || p->in[i] == '\t')) {
    sep = 1;
  }
  if (sep == 0) {
    return;
  }
  do {
    i += sep;
    if (i >= p->length) {
      return;
    }
    sep = xml_xml11_line_end_bytes(p, i);
    if (sep == 0 && (p->in[i] == ' ' || p->in[i] == '\t')) {
      sep = 1;
    }
  } while (sep != 0);
  if (p->length - i < 7 || memcmp(p->in + i, "version", 7) != 0) {
    return;
  }
  i += 7;
  while (i < p->length) {
    sep = xml_xml11_line_end_bytes(p, i);
    if (sep == 0 && (p->in[i] == ' ' || p->in[i] == '\t')) {
      sep = 1;
    }
    if (sep == 0) {
      break;
    }
    i += sep;
  }
  if (i >= p->length || p->in[i++] != '=') {
    return;
  }
  while (i < p->length) {
    sep = xml_xml11_line_end_bytes(p, i);
    if (sep == 0 && (p->in[i] == ' ' || p->in[i] == '\t')) {
      sep = 1;
    }
    if (sep == 0) {
      break;
    }
    i += sep;
  }
  if (i >= p->length || (p->in[i] != '\'' && p->in[i] != '"')) {
    return;
  }
  const unsigned char quote = p->in[i++];
  if (p->length - i >= 4 && memcmp(p->in + i, "1.1", 3) == 0
      && p->in[i + 3] == quote) {
    p->version = XML_VERSION_11;
  }
}

GTEXT_XML_Status xml_append_normalized(
    xml_ctx * p, markup_buf * buf, size_t from, size_t to) {
  const char * in = (const char *)p->in;
  size_t run = from;
  for (size_t i = from; i < to;) {
    const size_t eol = xml_line_end_bytes(p, i);
    if (eol == 0 || i + eol > to) {
      uint32_t cp = 0;
      size_t width = 1;
      const xml_char_status decoded = xml_decode(p, i, &cp, &width);
      if (decoded == XML_CHAR_BAD_UTF8 || decoded == XML_CHAR_EOF || width == 0) {
        width = 1;
      }
      i += width;
      continue;
    }
    GTEXT_XML_Status status = xml_append(p, buf, in + run, i - run);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    status = xml_append(p, buf, "\n", 1);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    i += eol;
    run = i;
  }
  return run < to ? xml_append(p, buf, in + run, to - run) : GTEXT_XML_OK;
}

/* ---- names -------------------------------------------------------------- */

/* 0 a name starts at `start` and *length is its bytes; 1 there is none; 2 it is
   longer than the limit. Never consumes. A character that ends the name because
   it is not a name character - including bytes that are not UTF-8 - is left for
   the caller to meet, which is when it is reported. */
static int name_at(const xml_ctx * p, size_t start, size_t * length) {
  size_t i = start;
  int first = 1;
  while (i < p->length) {
    const unsigned char b = p->in[i];
    if (b < 0x80) {
      const int letter = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')
          || b == '_' || b == ':';
      const int more = (b >= '0' && b <= '9') || b == '-' || b == '.';
      if (!(letter || (!first && more))) {
        break;
      }
      i++;
    }
    else {
      uint32_t cp = 0;
      size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        break;
      }
      if (first ? !markup_xml_name_start_char(cp) : !markup_xml_name_char(cp)) {
        break;
      }
      i += n;
    }
    first = 0;
    if (i - start > p->max_name_bytes) {
      return 2;
    }
  }
  if (i == start) {
    return 1;
  }
  *length = i - start;
  return 0;
}

GTEXT_XML_Status xml_scan_name(xml_ctx * p, size_t * length) {
  switch (name_at(p, p->pos, length)) {
    case 0:
      if (p->partial && xml_at_end(p, p->pos + *length)) {
        /* It may go on: a name that ends where the input does has not ended. */
        return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
      }
      return GTEXT_XML_OK;
    case 2:
      return xml_fail(p, GTEXT_XML_E_LIMIT_NAME, "name too long");
    default:
      return xml_unexpected(p, "a name was expected");
  }
}

/* ---- references --------------------------------------------------------- */

static int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

GTEXT_XML_Status xml_parse_reference(xml_ctx * p, markup_buf * buf) {
  const GTEXT_Markup_Location at = xml_here(p);
  const unsigned char * in = p->in;
  size_t i = p->pos + 1;

  if (i < p->length && in[i] == '#') {
    i++;
    const int hex = i < p->length && in[i] == 'x';
    if (hex) {
      i++;
    }
    uint32_t value = 0;
    size_t digits = 0;
    while (i < p->length) {
      const int d = hex ? hex_value(in[i])
                        : (in[i] >= '0' && in[i] <= '9' ? in[i] - '0' : -1);
      if (d < 0) {
        break;
      }
      /* Saturate above U+10FFFF; the value is refused below either way. */
      if (value <= 0x10FFFF) {
        value = value * (hex ? 16u : 10u) + (uint32_t)d;
      }
      digits++;
      i++;
    }
    if (digits == 0 || i >= p->length || in[i] != ';') {
      return xml_fail_at(p,
          i >= p->length ? GTEXT_XML_E_UNEXPECTED_EOF : GTEXT_XML_E_SYNTAX,
          "malformed character reference", &at);
    }
    if (!(p->version == XML_VERSION_11 ? is_xml11_char(value)
                                       : is_xml_char(value))) {
      return xml_fail_at(p, GTEXT_XML_E_CHARACTER,
          "character reference to a character XML does not allow", &at);
    }
    const GTEXT_XML_Status status = xml_append_code_point(p, buf, value);
    if (status != GTEXT_XML_OK) {
      return status;
    }
    xml_consume(p, i + 1 - p->pos);
    return GTEXT_XML_OK;
  }

  size_t length = 0;
  const int found = name_at(p, i, &length);
  if (found == 2) {
    return xml_fail_at(p, GTEXT_XML_E_LIMIT_NAME, "name too long", &at);
  }
  if (found != 0) {
    return xml_fail_at(p,
        xml_at_end(p, i) ? GTEXT_XML_E_UNEXPECTED_EOF : GTEXT_XML_E_SYNTAX,
        "'&' must begin a reference", &at);
  }
  const size_t end = i + length;
  if (xml_at_end(p, end) || in[end] != ';') {
    return xml_fail_at(p,
        xml_at_end(p, end) ? GTEXT_XML_E_UNEXPECTED_EOF : GTEXT_XML_E_SYNTAX,
        "reference not terminated by ';'", &at);
  }
  const char * name = (const char *)in + i;
  const char * replacement = NULL;
  size_t replacement_length = 0;
  const void * source_identity = NULL;
  const void * source_bytes = NULL;
  size_t source_length = 0;
  const int predefined = (length == 2 &&
          (memcmp(name, "lt", 2) == 0 || memcmp(name, "gt", 2) == 0))
      || (length == 3 && memcmp(name, "amp", 3) == 0)
      || (length == 4 &&
          (memcmp(name, "apos", 4) == 0 || memcmp(name, "quot", 4) == 0));
  const int parsed_source = p->dtd_mode == GTEXT_XML_DTD_PROCESS
      && !predefined && buf == &p->text
      && p->seams.open_entity_source
      && p->seams.open_entity_source(p->seams.user, name, length,
          &source_identity, &source_bytes, &source_length);
  if (parsed_source) {
    const GTEXT_XML_Status flushed = xml_flush_text(p);
    if (flushed != GTEXT_XML_OK) {
      return flushed;
    }
    xml_consume(p, end + 1 - p->pos);
    const GTEXT_XML_Status pushed = xml_source_push(p,
        XML_SOURCE_INTERNAL_ENTITY, source_identity, source_bytes, source_length);
    if (pushed != GTEXT_XML_OK) {
      return xml_fail_at(p, pushed,
          "could not enter parsed entity source", &at);
    }
    return GTEXT_XML_OK;
  }
  if (length == 2 && memcmp(name, "lt", 2) == 0) {
    replacement = "<";
  }
  else if (length == 2 && memcmp(name, "gt", 2) == 0) {
    replacement = ">";
  }
  else if (length == 3 && memcmp(name, "amp", 3) == 0) {
    replacement = "&";
  }
  else if (length == 4 && memcmp(name, "apos", 4) == 0) {
    replacement = "'";
  }
  else if (length == 4 && memcmp(name, "quot", 4) == 0) {
    replacement = "\"";
  }
  if (replacement) {
    replacement_length = 1;
  }
  else if (!p->seams.resolve_entity
      || !p->seams.resolve_entity(p->seams.user, name, length, &replacement,
          &replacement_length)) {
    return xml_fail_at(p, GTEXT_XML_E_ENTITY,
        "entity reference other than the five predefined ones", &at);
  }
  const GTEXT_XML_Status status =
      xml_append(p, buf, replacement, replacement_length);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  xml_consume(p, end + 1 - p->pos);
  return GTEXT_XML_OK;
}

/* ---- character data ----------------------------------------------------- */

GTEXT_XML_Status xml_scan_text_run(xml_ctx * p, size_t * end) {
  const unsigned char * in = p->in;
  size_t i = p->pos;
  while (i < p->length) {
    const unsigned char b = in[i];
    if (b == '<' || b == '&') {
      break;
    }
    if (b >= 0x20 && (b < 0x7F || (b == 0x7F && p->version == XML_VERSION_10))) {
      if (b == '>' && i >= p->pos + 2 && in[i - 1] == ']' && in[i - 2] == ']') {
        xml_consume(p, i - 2 - p->pos);
        return xml_fail(p, GTEXT_XML_E_SYNTAX,
            "\"]]>\" is not allowed in character data");
      }
      i++;
      continue;
    }
    if (b == '\t' || b == '\n' || b == '\r') {
      i++;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (b >= 0x80 && xml_decode(p, i, &cp, &n) == XML_CHAR_OK) {
      i += n;
      continue;
    }
    /* Go to the character and let it say what is wrong with it. */
    xml_consume(p, i - p->pos);
    return xml_unexpected(p, "invalid character");
  }
  *end = i;
  return GTEXT_XML_OK;
}

/* ---- a set that empties in constant time -------------------------------- */

uint32_t xml_hash(const void * bytes, size_t length, uint32_t seed) {
  const unsigned char * s = bytes;
  uint32_t h = 2166136261u ^ seed;
  for (size_t i = 0; i < length; i++) {
    h ^= s[i];
    h *= 16777619u;
  }
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  return h;
}

GTEXT_XML_Status xml_hset_begin(xml_ctx * p, xml_hset * set, size_t expected) {
  size_t want = 16;
  while (want < expected * 2) {
    want *= 2;
  }
  if (want > set->capacity) {
    xml_slot * grown =
        gtext_allocator_calloc(p->allocator, want, sizeof(xml_slot));
    if (!grown) {
      return xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
    }
    xml_release(p, set->slots);
    set->slots = grown;
    set->capacity = want;
    set->stamp = 0;
  }
  /* Bumping the stamp empties the set without touching the slots. Only when it
     wraps are they cleared, so that a slot written four billion elements ago
     cannot read as live. */
  if (++set->stamp == 0) {
    memset(set->slots, 0, set->capacity * sizeof(xml_slot));
    set->stamp = 1;
  }
  set->count = 0;
  return GTEXT_XML_OK;
}

void xml_hset_free(xml_ctx * p, xml_hset * set) {
  xml_release(p, set->slots);
  memset(set, 0, sizeof(*set));
}

static void hset_place(xml_hset * set, uint32_t hash, uint32_t index) {
  size_t slot = hash & (set->capacity - 1);
  while (set->slots[slot].stamp == set->stamp) {
    slot = (slot + 1) & (set->capacity - 1);
  }
  set->slots[slot].stamp = set->stamp;
  set->slots[slot].index = index;
  set->slots[slot].hash = hash;
}

int xml_hset_insert(xml_ctx * p, xml_hset * set, uint32_t hash, uint32_t index,
    xml_hset_same same, const void * key) {
  if ((set->count + 1) * 2 > set->capacity) {
    /* Rehash the live members into a table twice the size, under the same
       stamp. */
    const size_t want = set->capacity * 2;
    xml_slot * grown =
        gtext_allocator_calloc(p->allocator, want, sizeof(xml_slot));
    if (!grown) {
      xml_fail(p, GTEXT_XML_E_OOM, "out of memory");
      return -1;
    }
    xml_slot * old = set->slots;
    const size_t old_capacity = set->capacity;
    set->slots = grown;
    set->capacity = want;
    for (size_t i = 0; i < old_capacity; i++) {
      if (old[i].stamp == set->stamp) {
        hset_place(set, old[i].hash, old[i].index);
      }
    }
    xml_release(p, old);
  }
  size_t slot = hash & (set->capacity - 1);
  while (set->slots[slot].stamp == set->stamp) {
    if (set->slots[slot].hash == hash && same(p, set->slots[slot].index, key)) {
      return 1;
    }
    slot = (slot + 1) & (set->capacity - 1);
  }
  set->slots[slot].stamp = set->stamp;
  set->slots[slot].index = index;
  set->slots[slot].hash = hash;
  set->count++;
  return 0;
}
