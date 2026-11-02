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
 * `CSS.escape()` (css_cssom_util.h): CSSOM's "serialize an identifier" of well-formed text. The engine is css_serialize.c's; this adds the check that the text is
 * well-formed (UTF-8, or WTF-8 with its lone surrogates) and the buffer form.
 */

#include <string.h>

#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_internal.h"

/// Whether the bytes are well-formed UTF-8, or with `wtf8` WTF-8 (a surrogate code point in three bytes is allowed).
static bool well_formed(const unsigned char * s, size_t n, bool wtf8) {
  size_t i = 0;
  while (i < n) {
    const unsigned char b = s[i];
    if (b < 0x80) {
      i++;
      continue;
    }
    size_t need;
    unsigned char lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
      need = 1;
    }
    else if (b >= 0xE0 && b <= 0xEF) {
      need = 2;
      if (b == 0xE0) {
        lo = 0xA0;
      }
      else if (b == 0xED && !wtf8) {
        hi = 0x9F;
      }
    }
    else if (b >= 0xF0 && b <= 0xF4) {
      need = 3;
      if (b == 0xF0) {
        lo = 0x90;
      }
      else if (b == 0xF4) {
        hi = 0x8F;
      }
    }
    else {
      return false;
    }
    for (size_t k = 1; k <= need; k++) {
      if (i + k >= n || s[i + k] < lo || s[i + k] > hi) {
        return false;
      }
      lo = 0x80;
      hi = 0xBF;
    }
    i += need + 1;
  }
  return true;
}

GTEXT_CSS_Status gtext_cssom_escape(const char * text, size_t length, GTEXT_CSSOM_Text_Encoding encoding, const GTEXT_Markup_Sink * sink) {
  if ((!text && length) || !sink || !sink->write || (encoding != GTEXT_CSSOM_TEXT_UTF8 && encoding != GTEXT_CSSOM_TEXT_WTF8)) {
    return GTEXT_CSS_E_INVALID;
  }
  if (length == 0) {
    return GTEXT_CSS_OK;   /* the empty string escapes to the empty string */
  }
  const bool wtf8 = encoding == GTEXT_CSSOM_TEXT_WTF8;
  if (!well_formed((const unsigned char *)text, length, wtf8)) {
    return GTEXT_CSS_E_INVALID;
  }
  return css_serialize_identifier_text(text, length, wtf8, sink);
}

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
  bool oom;
} sbuf;

static GTEXT_Markup_Status sbuf_write(void * user, const char * data, size_t n) {
  sbuf * b = user;
  if (b->oom) {
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 32;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!grown) {
      b->oom = true;
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, data, n);
  b->len += n;
  b->data[b->len] = '\0';
  return GTEXT_MARKUP_OK;
}

GTEXT_CSS_Status gtext_cssom_escape_buffer(const char * text, size_t length, GTEXT_CSSOM_Text_Encoding encoding, const GTEXT_Allocator * allocator,
    char ** out, size_t * out_length) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  sbuf b;
  memset(&b, 0, sizeof b);
  b.alloc = *(allocator ? allocator : gtext_allocator_default());
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &b;
  sink.write = sbuf_write;
  GTEXT_CSS_Status st = gtext_cssom_escape(text, length, encoding, &sink);
  if (st == GTEXT_CSS_E_WRITE && b.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&b.alloc, b.data);
    return st;
  }
  if (!b.data) {
    b.data = gtext_allocator_malloc(&b.alloc, 1);
    if (!b.data) {
      return GTEXT_CSS_E_OOM;
    }
    b.data[0] = '\0';
  }
  *out = b.data;
  if (out_length) {
    *out_length = b.len;
  }
  return GTEXT_CSS_OK;
}
