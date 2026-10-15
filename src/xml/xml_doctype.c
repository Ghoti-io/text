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
 * The document type declaration and ordered internal syntax records.
 *
 * XML 1.0 (Fifth Edition) [28] doctypedecl, [75] ExternalID, [11]
 * SystemLiteral, [12] PubidLiteral, [28a]-[29] and the markup declarations of
 * the internal subset.
 *
 * The name and external identifiers are kept in a doctype node. In DTD
 * processing mode, each internal-subset construct is retained in source order
 * with its kind and physical location. Declaration evaluation is deliberately
 * separate; this scanner does not create effective binding indexes.
 */

#include <ghoti.io/text/macros.h>

#include <limits.h>
#include <string.h>

#include "xml_internal.h"

/* The specific code for plain syntax, the generic ones when the character
   itself is the trouble. */
static GTEXT_XML_Status bad(xml_ctx * p, const char * message) {
  uint32_t cp = 0;
  size_t n = 0;
  if (xml_decode(p, p->pos, &cp, &n) == XML_CHAR_OK) {
    return xml_fail(p, GTEXT_XML_E_DOCTYPE, message);
  }
  return xml_unexpected(p, message);
}

static int pubid_char(unsigned char c, unsigned char quote) {
  if (c == quote) {
    return 0;
  }
  return c == 0x20 || c == 0x0D || c == 0x0A || (c >= 'a' && c <= 'z')
      || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
      || (c != 0 && strchr("-'()+,./:=?;!*#@$_%", c) != NULL);
}

/* A quoted literal at the cursor. Appends its value to `out` - line ends
   normalized, and for a public identifier white space collapsed and trimmed as
   the specification asks processors to - and leaves the cursor after the
   closing quote. */
static GTEXT_XML_Status literal(xml_ctx * p, markup_buf * out, int is_public) {
  const unsigned char quote = p->pos < p->length ? p->in[p->pos] : 0;
  if (quote != '"' && quote != '\'') {
    return bad(p, "a quoted literal was expected");
  }
  XML_CONSUME_OR_RETURN(p, 1);
  /* An empty literal is present and empty, which is not the same as absent:
     make sure there is a string to hand over. */
  GTEXT_XML_Status status = xml_append(p, out, "", 0);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  const size_t start = p->pos;
  size_t i = start;
  while (i < p->length && p->in[i] != quote) {
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) {
      return work;
    }
    const unsigned char b = p->in[i];
    if (is_public) {
      if (!pubid_char(b, quote)) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return bad(p, "character not allowed in a public identifier");
      }
      i++;
      continue;
    }
    if (b >= 0x20 && b < 0x80
        && !(b == 0x7F && p->version == XML_VERSION_11)) {
      i++;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
      XML_CONSUME_OR_RETURN(p, i - p->pos);
      return xml_unexpected(p, "invalid character");
    }
    i += n;
  }
  if (i >= p->length) {
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated literal");
  }
  if (is_public) {
    size_t words = 0;
    size_t j = start;
    while (j < i) {
      status = xml_charge_work(p, 1);
      if (status != GTEXT_XML_OK) return status;
      while (j < i) {
        const size_t whitespace = xml_space_bytes(p, j);
        if (whitespace == 0 || j + whitespace > i) {
          break;
        }
        j += whitespace;
      }
      const size_t word = j;
      while (j < i && xml_space_bytes(p, j) == 0) {
        j++;
      }
      if (j > word) {
        if (words++ > 0) {
          status = xml_append(p, out, " ", 1);
          if (status != GTEXT_XML_OK) {
            return status;
          }
        }
        status = xml_append(p, out, (const char *)p->in + word, j - word);
        if (status != GTEXT_XML_OK) {
          return status;
        }
      }
    }
  }
  else {
    status = xml_append_normalized(p, out, start, i);
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
  XML_CONSUME_OR_RETURN(p, i + 1 - p->pos);
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_bad(xml_ctx * p, const char * message) {
  if (p->partial && p->pos >= p->length)
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
  return xml_fail(p, GTEXT_XML_E_DOCTYPE, message);
}

#define DTD_SKIP_SPACE_OR_RETURN(p) do { \
  xml_skip_space(p); \
  if ((p)->err.code != GTEXT_XML_OK) return (p)->err.code; \
} while (0)

static GTEXT_XML_Status dtd_need_space(xml_ctx * p) {
  size_t skipped = xml_skip_space(p);
  if (p->err.code != GTEXT_XML_OK) return p->err.code;
  if (skipped == 0) return dtd_bad(p, "white space is required in a DTD declaration");
  return p->err.code;
}

static GTEXT_XML_Status dtd_name(xml_ctx * p) {
  size_t n = 0;
  GTEXT_XML_Status s = xml_scan_name(p, &n);
  if (s != GTEXT_XML_OK) return s;
  return xml_consume(p, n);
}

static GTEXT_XML_Status dtd_quoted(xml_ctx * p, int is_public) {
  markup_buf tmp;
  markup_buf_init(&tmp, p->allocator);
  GTEXT_XML_Status s = literal(p, &tmp, is_public);
  markup_buf_free(&tmp);
  return s;
}

static int dtd_xml_char(const xml_ctx * p, uint32_t c);

/* EntityValue has reference syntax of its own. Validate references here but
   leave them lexical: this story retains DTD text and does not expand PEs. */
static GTEXT_XML_Status dtd_entity_value(xml_ctx * p) {
  if (p->pos >= p->length || (p->in[p->pos] != '\'' && p->in[p->pos] != '"'))
    return dtd_bad(p, "an entity value was expected");
  unsigned char quote = p->in[p->pos];
  XML_CONSUME_OR_RETURN(p, 1);
  while (p->pos < p->length && p->in[p->pos] != quote) {
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) return work;
    if (p->in[p->pos] == '&' || p->in[p->pos] == '%') {
      unsigned char sigil = p->in[p->pos];
      XML_CONSUME_OR_RETURN(p, 1);
      if (sigil == '&' && p->pos < p->length && p->in[p->pos] == '#') {
        XML_CONSUME_OR_RETURN(p, 1);
        int hex = p->pos < p->length && (p->in[p->pos] == 'x' || p->in[p->pos] == 'X');
        if (hex) XML_CONSUME_OR_RETURN(p, 1);
        size_t digits = 0;
        uint32_t value = 0;
        while (p->pos < p->length) {
          unsigned char c = p->in[p->pos];
          unsigned int digit = c >= '0' && c <= '9' ? (unsigned)(c - '0')
              : hex && c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
              : hex && c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : UINT_MAX;
          if (digit == UINT_MAX) break;
          if (value > (UINT32_MAX - digit) / (hex ? 16 : 10))
            return dtd_bad(p, "character reference is out of range");
          value = value * (hex ? 16 : 10) + digit;
          XML_CONSUME_OR_RETURN(p, 1); ++digits;
        }
        if (!digits) return dtd_bad(p, "a character reference needs digits");
        if (!dtd_xml_char(p, value)) return xml_fail(p, GTEXT_XML_E_CHARACTER,
            "character reference is not allowed in this XML version");
      }
      else {
        GTEXT_XML_Status s = dtd_name(p);
        if (s != GTEXT_XML_OK) return s;
      }
      if (p->pos >= p->length || p->in[p->pos] != ';') return dtd_bad(p, "a reference in an entity value ends with ';'");
      XML_CONSUME_OR_RETURN(p, 1);
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, p->pos, &cp, &n) != XML_CHAR_OK) return xml_unexpected(p, "invalid character in entity value");
    XML_CONSUME_OR_RETURN(p, n);
  }
  if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated entity value");
  return xml_consume(p, 1);
}

static int dtd_xml_char(const xml_ctx * p, uint32_t c) {
  if (p->version == XML_VERSION_11)
    return (c >= 1 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD)
        || (c >= 0x10000 && c <= 0x10FFFF);
  return c == 0x9 || c == 0xA || c == 0xD
      || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD)
      || (c >= 0x10000 && c <= 0x10FFFF);
}

static GTEXT_XML_Status dtd_attribute_value(xml_ctx * p) {
  if (p->pos >= p->length || (p->in[p->pos] != '\'' && p->in[p->pos] != '"'))
    return dtd_bad(p, "an attribute default value was expected");
  unsigned char quote = p->in[p->pos];
  XML_CONSUME_OR_RETURN(p, 1);
  while (p->pos < p->length && p->in[p->pos] != quote) {
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) return work;
    unsigned char c = p->in[p->pos];
    if (c == '<') return dtd_bad(p, "'<' is not allowed in an attribute default value");
    if (c == '&') {
      XML_CONSUME_OR_RETURN(p, 1);
      if (p->pos < p->length && p->in[p->pos] == '#') {
        XML_CONSUME_OR_RETURN(p, 1);
        int hex = p->pos < p->length && (p->in[p->pos] == 'x' || p->in[p->pos] == 'X');
        if (hex) XML_CONSUME_OR_RETURN(p, 1);
        size_t digits = 0;
        uint32_t value = 0;
        while (p->pos < p->length) {
          unsigned char d = p->in[p->pos];
          unsigned int n = d >= '0' && d <= '9' ? (unsigned)(d - '0')
              : hex && d >= 'a' && d <= 'f' ? (unsigned)(d - 'a' + 10)
              : hex && d >= 'A' && d <= 'F' ? (unsigned)(d - 'A' + 10) : UINT_MAX;
          if (n == UINT_MAX) break;
          if (value > (UINT32_MAX - n) / (hex ? 16 : 10))
            return dtd_bad(p, "character reference is out of range");
          value = value * (hex ? 16 : 10) + n;
          XML_CONSUME_OR_RETURN(p, 1); ++digits;
        }
        if (!digits) return dtd_bad(p, "a character reference needs digits");
        if (!dtd_xml_char(p, value)) return xml_fail(p, GTEXT_XML_E_CHARACTER,
            "character reference is not allowed in this XML version");
      }
      else {
        GTEXT_XML_Status s = dtd_name(p);
        if (s != GTEXT_XML_OK) return s;
      }
      if (p->pos >= p->length || p->in[p->pos] != ';')
        return dtd_bad(p, "a reference in an attribute default ends with ';'");
      XML_CONSUME_OR_RETURN(p, 1);
      continue;
    }
    uint32_t cp = 0; size_t n = 0;
    if (xml_decode(p, p->pos, &cp, &n) != XML_CHAR_OK)
      return xml_unexpected(p, "invalid character in attribute default value");
    XML_CONSUME_OR_RETURN(p, n);
  }
  if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
      "unterminated attribute default value");
  return xml_consume(p, 1);
}

static GTEXT_XML_Status dtd_nmtoken(xml_ctx * p) {
  size_t start = p->pos;
  while (p->pos < p->length) {
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, p->pos, &cp, &n) != XML_CHAR_OK || !markup_xml_name_char(cp)) break;
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) return work;
    XML_CONSUME_OR_RETURN(p, n);
    if (p->pos - start > p->max_name_bytes)
      return xml_fail(p, GTEXT_XML_E_LIMIT_NAME, "DTD Nmtoken exceeds the name byte limit");
  }
  if (p->pos == start) return dtd_bad(p, "an Nmtoken was expected");
  return GTEXT_XML_OK;
}

/* XML content particles are nested, but XML's grammar bounds their nesting by
   the parser's input/depth limits. This iterative form avoids C recursion. */
static GTEXT_XML_Status dtd_content_model_impl(xml_ctx * p,
    unsigned char * separators, size_t separator_capacity) {
  if (xml_looking_at(p, "EMPTY", 5)) return xml_consume(p, 5);
  if (xml_looking_at(p, "ANY", 3)) return xml_consume(p, 3);
  if (p->pos >= p->length || p->in[p->pos] != '(')
    return dtd_bad(p, "an element content specification was expected");
  XML_CONSUME_OR_RETURN(p, 1);
  DTD_SKIP_SPACE_OR_RETURN(p);
  if (xml_looking_at(p, "#PCDATA", 7)) {
    XML_CONSUME_OR_RETURN(p, 7);
    DTD_SKIP_SPACE_OR_RETURN(p);
    size_t names = 0;
    while (p->pos < p->length && p->in[p->pos] == '|') {
      XML_CONSUME_OR_RETURN(p, 1);
      DTD_SKIP_SPACE_OR_RETURN(p);
      GTEXT_XML_Status s = dtd_name(p);
      if (s != GTEXT_XML_OK) return s;
      ++names;
      DTD_SKIP_SPACE_OR_RETURN(p);
    }
    if (p->pos >= p->length || p->in[p->pos] != ')')
      return dtd_bad(p, "')' was expected to end a mixed content model");
    XML_CONSUME_OR_RETURN(p, 1);
    if (names != 0) {
      if (p->pos >= p->length || p->in[p->pos] != '*')
        return dtd_bad(p, "a mixed content choice must end with '*'");
      XML_CONSUME_OR_RETURN(p, 1);
    }
    else if (p->pos < p->length && p->in[p->pos] == '*') {
      XML_CONSUME_OR_RETURN(p, 1);
    }
    return GTEXT_XML_OK;
  }
  size_t depth = 1;
  int need_particle = 1;
  int separator = 0;
  int has_indicator = 0;
  for (;;) {
    DTD_SKIP_SPACE_OR_RETURN(p);
    if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated content model");
    unsigned char c = p->in[p->pos];
    if (need_particle) {
      if (c == '(') {
        XML_CONSUME_OR_RETURN(p, 1);
        if (depth >= p->max_depth || depth >= separator_capacity)
          return xml_fail(p, GTEXT_XML_E_LIMIT_DEPTH, "DTD content model depth limit exceeded");
        separators[depth] = 0;
        ++depth;
        need_particle = 1;
        separator = 0;
        has_indicator = 0;
        continue;
      }
      if (xml_looking_at(p, "#PCDATA", 7)) return dtd_bad(p, "#PCDATA is only legal in mixed content");
      GTEXT_XML_Status s = dtd_name(p);
      if (s != GTEXT_XML_OK) return s;
      need_particle = 0;
      has_indicator = 0;
      continue;
    }
    if (c == '?' || c == '*' || c == '+') {
      if (has_indicator) return dtd_bad(p, "a content particle may have only one occurrence indicator");
      has_indicator = 1;
      XML_CONSUME_OR_RETURN(p, 1);
      continue;
    }
    if (c == ',' || c == '|') {
      if (separator && separator != c) return dtd_bad(p, "a content group cannot mix ',' and '|' separators");
      separator = c;
      separators[depth - 1] = (unsigned char)c;
      XML_CONSUME_OR_RETURN(p, 1);
      need_particle = 1;
      has_indicator = 0;
      continue;
    }
    if (c == ')') {
      if (need_particle) return dtd_bad(p, "a content particle was expected before ')'");
      if (separators[depth - 1] == 0)
        return dtd_bad(p, "a children content group must contain ',' or '|'");
      XML_CONSUME_OR_RETURN(p, 1);
      if (--depth == 0) break;
      separator = separators[depth - 1];
      need_particle = 0;
      has_indicator = 0;
      continue;
    }
    return dtd_bad(p, "unexpected token in an element content model");
  }
  if (xml_looking_at(p, "?", 1) || xml_looking_at(p, "*", 1) || xml_looking_at(p, "+", 1))
    XML_CONSUME_OR_RETURN(p, 1);
  return GTEXT_XML_OK;
}

static GTEXT_XML_Status dtd_content_model(xml_ctx * p) {
  size_t capacity = p->length / 2 + 1;
  if (capacity > p->max_depth) capacity = p->max_depth;
  if (capacity == 0) return xml_fail(p, GTEXT_XML_E_LIMIT_DEPTH,
      "DTD content model depth limit exceeded");
  if (capacity > SIZE_MAX / sizeof(unsigned char))
    return xml_fail(p, GTEXT_XML_E_OOM, "DTD content model stack is too large");
  unsigned char * separators = xml_alloc(p, capacity * sizeof(*separators));
  if (!separators) return xml_fail(p, GTEXT_XML_E_OOM,
      "could not allocate DTD content model stack");
  memset(separators, 0, capacity * sizeof(*separators));
  GTEXT_XML_Status status = dtd_content_model_impl(p, separators, capacity);
  xml_release(p, separators);
  return status;
}

static GTEXT_XML_Status dtd_attribute_type(xml_ctx * p) {
  static const char * const words[] = {"CDATA", "IDREFS", "IDREF", "ID", "ENTITIES", "ENTITY", "NMTOKENS", "NMTOKEN"};
  int notation = 0;
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
    size_t n = strlen(words[i]);
    if (xml_looking_at(p, words[i], n)) return xml_consume(p, n);
  }
  if (xml_looking_at(p, "NOTATION", 8)) {
    notation = 1;
    GTEXT_XML_Status s = xml_consume(p, 8);
    if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p);
    if (s != GTEXT_XML_OK) return s;
  }
  if (p->pos >= p->length || p->in[p->pos] != '(') return dtd_bad(p, "an attribute type was expected");
  XML_CONSUME_OR_RETURN(p, 1);
  DTD_SKIP_SPACE_OR_RETURN(p);
  GTEXT_XML_Status s = notation ? dtd_name(p) : dtd_nmtoken(p);
  if (s != GTEXT_XML_OK) return s;
  DTD_SKIP_SPACE_OR_RETURN(p);
  while (p->pos < p->length && p->in[p->pos] == '|') {
    XML_CONSUME_OR_RETURN(p, 1); DTD_SKIP_SPACE_OR_RETURN(p);
    s = notation ? dtd_name(p) : dtd_nmtoken(p); if (s != GTEXT_XML_OK) return s;
    DTD_SKIP_SPACE_OR_RETURN(p);
  }
  if (p->pos >= p->length || p->in[p->pos] != ')') return dtd_bad(p, "')' was expected to end an attribute type");
  return xml_consume(p, 1);
}

static GTEXT_XML_Status dtd_default_decl(xml_ctx * p) {
  if (xml_looking_at(p, "#REQUIRED", 9)) return xml_consume(p, 9);
  if (xml_looking_at(p, "#IMPLIED", 8)) return xml_consume(p, 8);
  if (xml_looking_at(p, "#FIXED", 6)) {
    GTEXT_XML_Status s = xml_consume(p, 6);
    if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p);
    if (s != GTEXT_XML_OK) return s;
    return dtd_attribute_value(p);
  }
  return dtd_attribute_value(p);
}

static GTEXT_XML_Status dtd_external_id(xml_ctx * p, int allow_public_only) {
  GTEXT_XML_Status s;
  if (xml_looking_at(p, "SYSTEM", 6)) {
    s = xml_consume(p, 6); if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    return dtd_quoted(p, 0);
  }
  if (!xml_looking_at(p, "PUBLIC", 6)) return dtd_bad(p, "SYSTEM or PUBLIC was expected");
  s = xml_consume(p, 6); if (s != GTEXT_XML_OK) return s;
  s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
  s = dtd_quoted(p, 1); if (s != GTEXT_XML_OK) return s;
  size_t space = xml_skip_space(p);
  if (p->err.code != GTEXT_XML_OK) return p->err.code;
  if (allow_public_only && (p->pos >= p->length || p->in[p->pos] == '>')) return GTEXT_XML_OK;
  if (space == 0) return dtd_bad(p, "white space and a system literal were expected after a public identifier");
  return dtd_quoted(p, 0);
}

/* Parse one complete declaration body (cursor is immediately after keyword). */
static GTEXT_XML_Status validate_declaration(xml_ctx * p, int kind) {
  GTEXT_XML_Status s;
  /* The caller has checked that S follows the keyword; consume it here. */
  DTD_SKIP_SPACE_OR_RETURN(p);
  if (kind == 0) { /* ELEMENT */
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_content_model(p); if (s != GTEXT_XML_OK) return s;
  }
  else if (kind == 1) { /* ATTLIST */
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
    size_t initial_space = xml_skip_space(p);
    if (p->err.code != GTEXT_XML_OK) return p->err.code;
    if ((p->pos >= p->length || p->in[p->pos] != '>') && initial_space == 0)
      return dtd_bad(p, "white space is required before an attribute definition");
    size_t attrs = 0;
    for (;;) {
      size_t space = xml_skip_space(p);
      if (p->err.code != GTEXT_XML_OK) return p->err.code;
      if (p->pos < p->length && p->in[p->pos] == '>') {
        return xml_consume(p, 1);
      }
      if (attrs != 0 && !space) return dtd_bad(p, "white space is required between attribute definitions");
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
      s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
      s = dtd_attribute_type(p); if (s != GTEXT_XML_OK) return s;
      s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
      s = dtd_default_decl(p); if (s != GTEXT_XML_OK) return s;
      attrs++;
    }
  }
  else if (kind == 2) { /* ENTITY */
    int parameter_entity = 0;
    if (p->pos < p->length && p->in[p->pos] == '%') {
      parameter_entity = 1;
      XML_CONSUME_OR_RETURN(p, 1);
      s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    }
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    if (p->pos < p->length && (p->in[p->pos] == '\'' || p->in[p->pos] == '"')) {
      s = dtd_entity_value(p);
    }
    else {
      s = dtd_external_id(p, 0);
      DTD_SKIP_SPACE_OR_RETURN(p);
      if (s == GTEXT_XML_OK && xml_looking_at(p, "NDATA", 5)) {
        if (parameter_entity) return dtd_bad(p, "a parameter entity cannot have NDATA");
        s = xml_consume(p, 5);
        if (s == GTEXT_XML_OK) s = dtd_need_space(p);
        if (s == GTEXT_XML_OK) s = dtd_name(p);
      }
    }
    if (s != GTEXT_XML_OK) return s;
  }
  else { /* NOTATION */
    s = dtd_name(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_need_space(p); if (s != GTEXT_XML_OK) return s;
    s = dtd_external_id(p, 1); if (s != GTEXT_XML_OK) return s;
  }
  DTD_SKIP_SPACE_OR_RETURN(p);
  if (p->pos >= p->length || p->in[p->pos] != '>') return dtd_bad(p, "'>' was expected to end the DTD declaration");
  return xml_consume(p, 1);
}

/* DTD-off preserves the original skip-only behavior. DTD processing performs
   production validation above, but neither mode evaluates declarations. */
static GTEXT_XML_Status skip_declaration(xml_ctx * p, int is_entity) {
  for (;;) {
    size_t i = p->pos;
    while (i < p->length) {
      GTEXT_XML_Status work = xml_charge_work(p, 1);
      if (work != GTEXT_XML_OK) return work;
      unsigned char b = p->in[i];
      if (b == '"' || b == '\'' || b == '>' || b == '<' || b == ']' || b == '%') break;
      if ((b >= 0x20 && b < 0x80 && !(b == 0x7F && p->version == XML_VERSION_11))
          || b == '\t' || b == '\n' || b == '\r') { ++i; continue; }
      uint32_t cp = 0; size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return xml_unexpected(p, "invalid character");
      }
      i += n;
    }
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
        "unterminated declaration in the internal subset");
    unsigned char b = p->in[p->pos];
    if (b == '>') return xml_consume(p, 1);
    if (b == '<' || b == ']') return dtd_bad(p,
        "declaration in the internal subset is not terminated by '>'");
    if (b == '%' && p->partial && p->pos + 1 >= p->length)
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    if (b == '%' && is_entity && p->pos + 1 < p->length
        && xml_space_bytes(p, p->pos + 1) > 0) {
      XML_CONSUME_OR_RETURN(p, 1);
      continue;
    }
    if (b == '%') return dtd_bad(p,
        "a parameter entity reference cannot occur inside a declaration in the internal subset");
    unsigned char quote = b;
    XML_CONSUME_OR_RETURN(p, 1);
    i = p->pos;
    while (i < p->length && p->in[i] != quote) {
      GTEXT_XML_Status work = xml_charge_work(p, 1);
      if (work != GTEXT_XML_OK) return work;
      unsigned char c = p->in[i];
      if ((c >= 0x20 && c < 0x80 && !(c == 0x7F && p->version == XML_VERSION_11))
          || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
      uint32_t cp = 0; size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return xml_unexpected(p, "invalid character");
      }
      i += n;
    }
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    if (p->pos >= p->length) return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
        "unterminated literal in the internal subset");
    XML_CONSUME_OR_RETURN(p, 1);
  }
}

#undef DTD_SKIP_SPACE_OR_RETURN

static GTEXT_XML_Status retain_syntax(xml_ctx * p, GTEXT_Markup_Node * doctype,
    GTEXT_Markup_DTD_Syntax_Kind kind, size_t start,
    const GTEXT_Markup_Location * at) {
  if (!doctype || p->stream) return GTEXT_XML_OK;
  GTEXT_Markup_Status status = markup_doctype_append_syntax(doctype, kind,
      (const char *)p->in + start, p->pos - start, at);
  return xml_from_markup(p, status);
}

/* Past the '[' to past the ']'. */
static GTEXT_XML_Status skip_internal_subset(xml_ctx * p,
    GTEXT_Markup_Node * doctype) {
  static const char * const keywords[] = {
      "<!ELEMENT", "<!ATTLIST", "<!ENTITY", "<!NOTATION"};
  for (;;) {
    if (p->err.code != GTEXT_XML_OK) {
      return p->err.code;
    }
    xml_skip_space(p);
    if (p->err.code != GTEXT_XML_OK) {
      return p->err.code;
    }
    if (p->pos >= p->length) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "unterminated internal subset");
    }
    const unsigned char b = p->in[p->pos];
    GTEXT_XML_Status status;
    if (b == ']') {
      XML_CONSUME_OR_RETURN(p, 1);
      return GTEXT_XML_OK;
    }
    if (b == '%') {
      const size_t start = p->pos;
      const GTEXT_Markup_Location at = xml_here(p);
      status = xml_charge(p, &p->references, p->max_references, 1,
          GTEXT_XML_E_LIMIT_REFERENCES, "XML reference limit exceeded");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, 1);
      size_t length = 0;
      status = xml_scan_name(p, &length);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, length);
      status = xml_expect(p, ";", 1, "a parameter entity reference ends with ';'");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      status = retain_syntax(p, doctype,
          GTEXT_MARKUP_DTD_PARAMETER_REFERENCE, start, &at);
      if (status != GTEXT_XML_OK) return status;
      continue;
    }
    if (b != '<') {
      return bad(p, "unexpected content in the internal subset");
    }
    if (xml_cut_literal(p, "<!--", 4) || xml_cut_literal(p, "<?", 2)) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    }
    const size_t start = p->pos;
    const GTEXT_Markup_Location at = xml_here(p);
    GTEXT_Markup_DTD_Syntax_Kind kind;
    if (xml_looking_at(p, "<!--", 4)) {
      kind = GTEXT_MARKUP_DTD_COMMENT;
      status = xml_parse_comment(p, 0);
    }
    else if (xml_looking_at(p, "<?", 2)) {
      kind = GTEXT_MARKUP_DTD_PROCESSING_INSTRUCTION;
      status = xml_parse_pi(p, 0);
    }
    else {
      size_t k = 0;
      for (; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
        if (xml_looking_at(p, keywords[k], strlen(keywords[k]))) {
          break;
        }
        if (xml_cut_literal(p, keywords[k], strlen(keywords[k]))) {
          return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
        }
      }
      if (k == sizeof(keywords) / sizeof(keywords[0])) {
        return xml_fail(p, GTEXT_XML_E_DOCTYPE,
            "not a declaration, comment or processing instruction");
      }
      kind = (GTEXT_Markup_DTD_Syntax_Kind)k;
      status = xml_charge(p, &p->declarations, p->max_declarations, 1,
          GTEXT_XML_E_LIMIT_DECLARATIONS, "DTD declaration limit exceeded");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, strlen(keywords[k]));
      if (p->pos >= p->length || xml_space_bytes(p, p->pos) == 0) {
        return bad(p, "white space is required after the declaration keyword");
      }
      status = p->dtd_mode == GTEXT_XML_DTD_PROCESS
          ? validate_declaration(p, (int)k)
          : skip_declaration(p, k == 2);
    }
    if (status != GTEXT_XML_OK) {
      return status;
    }
    status = retain_syntax(p, doctype, kind, start, &at);
    if (status != GTEXT_XML_OK) return status;
  }
}

GTEXT_XML_Status xml_parse_doctype(xml_ctx * p) {
  const GTEXT_Markup_Location start = xml_here(p);
  markup_buf name;
  markup_buf pub;
  markup_buf sys;
  markup_buf_init(&name, p->allocator);
  markup_buf_init(&pub, p->allocator);
  markup_buf_init(&sys, p->allocator);
  int has_public = 0;
  int has_system = 0;
  GTEXT_Markup_Node * doctype = NULL;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  size_t length = 0;

  status = xml_consume(p, 9); /* "<!DOCTYPE" */
  if (status != GTEXT_XML_OK) goto done;
  if (xml_skip_space(p) == 0) {
    status = bad(p, "white space is required after <!DOCTYPE");
    goto done;
  }
  status = xml_scan_name(p, &length);
  if (status != GTEXT_XML_OK) {
    goto done;
  }
  status = xml_append(p, &name, (const char *)p->in + p->pos, length);
  if (status != GTEXT_XML_OK) {
    goto done;
  }
  status = xml_consume(p, length);
  if (status != GTEXT_XML_OK) goto done;
  size_t space = xml_skip_space(p);

  if (xml_cut_literal(p, "SYSTEM", 6) || xml_cut_literal(p, "PUBLIC", 6)) {
    status = xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    goto done;
  }
  const int is_system = xml_looking_at(p, "SYSTEM", 6);
  const int is_public = !is_system && xml_looking_at(p, "PUBLIC", 6);
  if (is_system || is_public) {
    if (space == 0) {
      status = bad(p, "white space is required before the external identifier");
      goto done;
    }
    status = xml_consume(p, 6);
    if (status != GTEXT_XML_OK) goto done;
    if (xml_skip_space(p) == 0) {
      status = bad(p, "white space is required after SYSTEM or PUBLIC");
      goto done;
    }
    if (is_public) {
      status = literal(p, &pub, 1);
      if (status != GTEXT_XML_OK) {
        goto done;
      }
      has_public = 1;
      if (xml_skip_space(p) == 0) {
        status = bad(p, "white space is required before the system identifier");
        goto done;
      }
    }
    status = literal(p, &sys, 0);
    if (status != GTEXT_XML_OK) {
      goto done;
    }
    has_system = 1;
    space = xml_skip_space(p);
  }
  (void)space;

  if (p->dtd_mode == GTEXT_XML_DTD_PROCESS && !p->stream) {
    status = xml_from_markup(p, gtext_markup_create_doctype(p->doc, name.data,
        has_public ? pub.data : NULL, has_system ? sys.data : NULL, &doctype));
    if (status != GTEXT_XML_OK) goto done;
  }

  if (p->pos < p->length && p->in[p->pos] == '[') {
    status = xml_consume(p, 1);
    if (status != GTEXT_XML_OK) goto done;
    status = skip_internal_subset(p, doctype);
    if (status != GTEXT_XML_OK) {
      goto done;
    }
    xml_skip_space(p);
  }
  if (p->pos >= p->length || p->in[p->pos] != '>') {
    status = bad(p, "'>' was expected to end the document type declaration");
    goto done;
  }
  status = xml_consume(p, 1);
  if (status != GTEXT_XML_OK) goto done;

  if (p->stream) {
    status = xml_emit_doctype(p, &start, name.data,
        has_public ? pub.data : NULL, has_system ? sys.data : NULL);
  }
  else {
    GTEXT_Markup_Node * node = doctype;
    if (!node) status = xml_from_markup(p,
        gtext_markup_create_doctype(p->doc, name.data,
            has_public ? pub.data : NULL, has_system ? sys.data : NULL, &node));
    if (status == GTEXT_XML_OK) {
      xml_attach(p->doc->root, node, &start);
    }
  }

done:
  if (status != GTEXT_XML_OK && doctype) {
    gtext_markup_node_free(doctype);
  }
  markup_buf_free(&name);
  markup_buf_free(&pub);
  markup_buf_free(&sys);
  return status;
}
