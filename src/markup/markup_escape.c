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
 * Escaping text for XML, the lexical checks a writer needs to decide whether a
 * piece of text can be written at all, and XML's name productions.
 *
 * XML 1.0 (Fifth Edition), productions [2] Char, [4]-[5] Name, [15] Comment,
 * [16] PI, [18] CDSect and [43]-[44] content references; and Namespaces in XML
 * 1.0 (Third Edition) [4] NCName.
 */

#include <ghoti.io/text/macros.h>

#include <ghoti.io/unicode/utf.h>
#include <stdint.h>
#include <string.h>

#include "markup_internal.h"

/* ---- characters --------------------------------------------------------- */

GTEXT_Markup_Status gtext_markup_check_xml_chars(const char * text, size_t len) {
  GTEXT_Markup_Status status = markup_check_utf8(text, len);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  const unsigned char * p = (const unsigned char *)text;
  for (size_t i = 0; i < len; i++) {
    /* The only characters that are well-formed UTF-8 and not an XML Char are
       the C0 controls other than tab, line feed and carriage return, and
       U+FFFE and U+FFFF; the surrogates cannot occur in valid UTF-8. */
    if (p[i] < 0x20 && p[i] != 0x09 && p[i] != 0x0A && p[i] != 0x0D) {
      return GTEXT_MARKUP_E_UNREPRESENTABLE;
    }
    if (p[i] == 0xEF && i + 2 < len && p[i + 1] == 0xBF
        && (p[i + 2] == 0xBE || p[i + 2] == 0xBF)) {
      return GTEXT_MARKUP_E_UNREPRESENTABLE;
    }
  }
  return GTEXT_MARKUP_OK;
}

static bool contains(const char * text, size_t len, const char * what,
    size_t what_len) {
  if (len < what_len) {
    return false;
  }
  for (size_t i = 0; i + what_len <= len; i++) {
    if (memcmp(text + i, what, what_len) == 0) {
      return true;
    }
  }
  return false;
}

GTEXT_Markup_Status gtext_markup_check_xml_comment(const char * text, size_t len) {
  const GTEXT_Markup_Status status = gtext_markup_check_xml_chars(text, len);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  if (contains(text, len, "--", 2) || (len > 0 && text[len - 1] == '-')) {
    return GTEXT_MARKUP_E_UNREPRESENTABLE;
  }
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_check_xml_pi_data(const char * text, size_t len) {
  const GTEXT_Markup_Status status = gtext_markup_check_xml_chars(text, len);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  return contains(text, len, "?>", 2) ? GTEXT_MARKUP_E_UNREPRESENTABLE
                                      : GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_check_xml_cdata(const char * text, size_t len) {
  const GTEXT_Markup_Status status = gtext_markup_check_xml_chars(text, len);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  return contains(text, len, "]]>", 3) ? GTEXT_MARKUP_E_UNREPRESENTABLE
                                       : GTEXT_MARKUP_OK;
}

/* ---- escaping ----------------------------------------------------------- */

/* What, if anything, a byte is replaced with in a mode. Only ASCII bytes are
   ever replaced, so the bytes of a multi-byte character pass through whole. */
static const char * replacement(GTEXT_Markup_Escape_Mode mode, unsigned char c) {
  switch (c) {
    case '&':
      return "&amp;";
    case '<':
      return "&lt;";
    case '>':
      return "&gt;";
    case '\r':
      return "&#13;";
    case '"':
      return mode == GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE ? "&quot;" : NULL;
    case '\t':
      return mode == GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE ? "&#9;" : NULL;
    case '\n':
      return mode == GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE ? "&#10;" : NULL;
    default:
      return NULL;
  }
}

GTEXT_Markup_Status gtext_markup_escape(GTEXT_Markup_Escape_Mode mode,
    const char * text, size_t len, const GTEXT_Markup_Sink * sink) {
  if (!sink || !sink->write || (!text && len != 0)
      || (mode != GTEXT_MARKUP_ESCAPE_XML_TEXT
          && mode != GTEXT_MARKUP_ESCAPE_XML_ATTRIBUTE)) {
    return GTEXT_MARKUP_E_INVALID;
  }
  /* Checked whole before anything is written: a refusal that had already sent
     half the text would leave the caller holding output nobody asked for. */
  GTEXT_Markup_Status status = gtext_markup_check_xml_chars(text, len);
  if (status != GTEXT_MARKUP_OK) {
    return status;
  }
  size_t run = 0;
  for (size_t i = 0; i < len; i++) {
    const char * with = replacement(mode, (unsigned char)text[i]);
    if (!with) {
      continue;
    }
    if (i > run) {
      status = sink->write(sink->user, text + run, i - run);
      if (status != GTEXT_MARKUP_OK) {
        return status;
      }
    }
    status = sink->write(sink->user, with, strlen(with));
    if (status != GTEXT_MARKUP_OK) {
      return status;
    }
    run = i + 1;
  }
  if (len > run) {
    status = sink->write(sink->user, text + run, len - run);
  }
  return status;
}

GTEXT_Markup_Status gtext_markup_escape_alloc(GTEXT_Markup_Escape_Mode mode,
    const char * text, size_t len, const GTEXT_Allocator * allocator, char ** out,
    size_t * out_len) {
  if (!out) {
    return GTEXT_MARKUP_E_INVALID;
  }
  markup_buf buf;
  markup_buf_init(&buf, allocator);
  GTEXT_Markup_Status status = markup_buf_append(&buf, "", 0);
  if (status == GTEXT_MARKUP_OK) {
    const GTEXT_Markup_Sink sink = {markup_buf_sink_write, &buf};
    status = gtext_markup_escape(mode, text, len, &sink);
  }
  if (status != GTEXT_MARKUP_OK) {
    markup_buf_free(&buf);
    return status;
  }
  *out = buf.data;
  if (out_len) {
    *out_len = buf.length;
  }
  return GTEXT_MARKUP_OK;
}

/* ---- names -------------------------------------------------------------- */

/* XML 1.0 (Fifth Edition) production [4]. */
bool markup_xml_name_start_char(uint32_t c) {
  return c == ':' || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z')
      || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6)
      || (c >= 0xF8 && c <= 0x2FF) || (c >= 0x370 && c <= 0x37D)
      || (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D)
      || (c >= 0x2070 && c <= 0x218F) || (c >= 0x2C00 && c <= 0x2FEF)
      || (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF)
      || (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

/* Production [4a]. */
bool markup_xml_name_char(uint32_t c) {
  return markup_xml_name_start_char(c) || c == '-' || c == '.' || (c >= '0' && c <= '9')
      || c == 0xB7 || (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}

static bool is_name(const char * text, size_t len, bool allow_colon) {
  if (!text || len == 0) {
    return false;
  }
  size_t i = 0;
  bool first = true;
  while (i < len) {
    uint32_t cp = 0;
    bool valid = false;
    const size_t used = guni_utf8_decode(text + i, len - i, &cp, &valid);
    if (!valid || used == 0) {
      return false;
    }
    if (!allow_colon && cp == ':') {
      return false;
    }
    if (first ? !markup_xml_name_start_char(cp) : !markup_xml_name_char(cp)) {
      return false;
    }
    first = false;
    i += used;
  }
  return true;
}

bool gtext_markup_is_xml_name(const char * text, size_t len) {
  return is_name(text, len, true);
}

bool gtext_markup_is_xml_ncname(const char * text, size_t len) {
  return is_name(text, len, false);
}
