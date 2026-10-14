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
 * The Encoding Standard's names and labels, and the pieces the decoders and
 * encoders share: statuses, the output buffer and the buffer sink.
 */

#include <ghoti.io/text/encoding.h>

#include "encoding_internal.h"

#include <stdint.h>
#include <string.h>

/* ---- statuses --------------------------------------------------------------------------- */

const char * gtext_encoding_status_string(GTEXT_Encoding_Status status) {
  switch (status) {
  case GTEXT_ENCODING_OK:
    return "ok";
  case GTEXT_ENCODING_E_INVALID:
    return "invalid argument";
  case GTEXT_ENCODING_E_OOM:
    return "out of memory";
  case GTEXT_ENCODING_E_OPTIONS:
    return "an option value out of range";
  case GTEXT_ENCODING_E_UNSUPPORTED:
    return "no codec for this encoding in this release";
  case GTEXT_ENCODING_E_NO_ENCODER:
    return "this encoding has no encoder";
  case GTEXT_ENCODING_E_MALFORMED:
    return "malformed input";
  case GTEXT_ENCODING_E_UNENCODABLE:
    return "a code point the encoding cannot represent";
  case GTEXT_ENCODING_E_LIMIT_OUTPUT:
    return "output limit exceeded";
  case GTEXT_ENCODING_E_WRITE:
    return "the sink refused output";
  case GTEXT_ENCODING_E_INTERNAL:
    return "internal error: a defect of this library";
  default:
    return "unknown status";
  }
}

/* ---- names and labels -------------------------------------------------------------------- */

static bool valid_encoding(GTEXT_Encoding e) {
  return (int)e > (int)GTEXT_ENCODING_NONE && (int)e < (int)GTEXT_ENCODING_COUNT;
}

const char * gtext_encoding_name(GTEXT_Encoding encoding) {
  return valid_encoding(encoding) ? gtext_encoding_names[encoding] : NULL;
}

/// The standard's ASCII whitespace: tab, line feed, form feed, carriage return, space.
static bool ascii_whitespace(unsigned char c) {
  return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

GTEXT_Encoding gtext_encoding_for_label(const char * label, size_t len) {
  if (!label) {
    return GTEXT_ENCODING_NONE;
  }
  const unsigned char * p = (const unsigned char *)label;
  while (len > 0 && ascii_whitespace(p[0])) {
    p++;
    len--;
  }
  while (len > 0 && ascii_whitespace(p[len - 1])) {
    len--;
  }
  unsigned char key[GTEXT_ENCODING_LABEL_KEY];
  if (len == 0 || len > gtext_encoding_label_longest || len > sizeof key) {
    return GTEXT_ENCODING_NONE;
  }
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = p[i];
    key[i] = (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
  }
  size_t lo = 0;
  size_t hi = gtext_encoding_label_total;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const GTEXT_Encoding_Label_Entry * e = &gtext_encoding_label_table[mid];
    const size_t common = e->length < len ? e->length : len;
    int cmp = memcmp(&gtext_encoding_label_pool[e->offset], key, common);
    if (cmp == 0) {
      cmp = e->length < len ? -1 : (e->length > len ? 1 : 0);
    }
    if (cmp == 0) {
      return (GTEXT_Encoding)e->encoding;
    }
    if (cmp < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return GTEXT_ENCODING_NONE;
}

bool gtext_encoding_has_encoder(GTEXT_Encoding encoding) {
  return valid_encoding(encoding) && gtext_encoding_enc_codec(encoding) != NULL;
}

size_t gtext_encoding_label_count(void) {
  return gtext_encoding_label_total;
}

bool gtext_encoding_label_at(size_t index, const char ** label, size_t * len, GTEXT_Encoding * encoding) {
  if (index >= gtext_encoding_label_total) {
    return false;
  }
  const GTEXT_Encoding_Label_Entry * e = &gtext_encoding_label_table[index];
  if (label) {
    *label = &gtext_encoding_label_pool[e->offset];
  }
  if (len) {
    *len = e->length;
  }
  if (encoding) {
    *encoding = (GTEXT_Encoding)e->encoding;
  }
  return true;
}

bool gtext_encoding_is_ascii_compatible(GTEXT_Encoding encoding) {
  if (!valid_encoding(encoding)) {
    return false;
  }
  switch (encoding) {
  case GTEXT_ENCODING_UTF_16BE:
  case GTEXT_ENCODING_UTF_16LE:
  case GTEXT_ENCODING_ISO_2022_JP:
  case GTEXT_ENCODING_REPLACEMENT:
    return false;
  default:
    return true;
  }
}

GTEXT_Encoding gtext_encoding_output_encoding(GTEXT_Encoding encoding) {
  if (!valid_encoding(encoding)) {
    return GTEXT_ENCODING_NONE;
  }
  if (encoding == GTEXT_ENCODING_REPLACEMENT || encoding == GTEXT_ENCODING_UTF_16BE
      || encoding == GTEXT_ENCODING_UTF_16LE) {
    return GTEXT_ENCODING_UTF_8;
  }
  return encoding;
}

bool gtext_encoding_is_supported(GTEXT_Encoding encoding) {
  return valid_encoding(encoding) && gtext_encoding_dec_codec(encoding) != NULL;
}

/* ---- the output buffer -------------------------------------------------------------------- */

/// Give the buffered bytes to the sink; false (with `status` set) if it refused. The buffer is empty afterwards either way, so a sink that refused is not asked again.
/* The chunk a sink is given is the one the public constant promises: callers size what they keep for each byte of it by that. */
_Static_assert(sizeof(((GTEXT_Out *)0)->buf) == GTEXT_ENCODING_MAX_CHUNK, "GTEXT_ENCODING_MAX_CHUNK is the size of the chunk buffer");

bool gtext_out_flush(GTEXT_Out * out) {
  if (out->n == 0) {
    return true;
  }
  const size_t n = out->n;
  out->n = 0;
  bool accepted;
  if (out->msink) {
    GTEXT_Encoding_Source_Map map;
    map.offsets = out->off;
    map.replaced = out->rep;
    map.widths = out->wid;
    accepted = out->msink->write(out->msink->user, (const char *)out->buf, n, &map);
  } else {
    accepted = out->sink->write(out->sink->user, (const char *)out->buf, n);
  }
  if (!accepted) {
    out->status = GTEXT_ENCODING_E_WRITE;
    return false;
  }
  out->delivered += n;
  return true;
}

/// Add a run of any length, in pieces; a run that does not fit is delivered as far as it does.
bool gtext_out_put_run(GTEXT_Out * out, const unsigned char * bytes, size_t n, size_t * accepted) {
  size_t taken = 0;
  *accepted = 0;
  /* A run that does not fit is delivered as far as it does, so that the sink has the longest legal prefix. */
  const size_t room_total = out->limit - out->total;
  const bool limited = n > room_total;
  if (limited) {
    n = room_total;
  }
  while (n > 0) {
    size_t room = GTEXT_ENCODING_CHUNK - out->n;
    if (room == 0) {
      if (!gtext_out_flush(out)) {
        *accepted = taken;
        return false;
      }
      room = GTEXT_ENCODING_CHUNK;
    }
    const size_t take = n < room ? n : room;
    memcpy(out->buf + out->n, bytes, take);
    if (out->off) {
      for (size_t k = 0; k < take; k++) {
        out->off[out->n + k] = out->cur_off + taken + k;
        out->rep[out->n + k] = 0;
        out->wid[out->n + k] = 1; /* (a run of ASCII is one byte a code point: only codecs that read ASCII that way are run so) */
      }
    }
    out->n += take;
    out->total += take;
    bytes += take;
    n -= take;
    taken += take;
  }
  *accepted = taken;
  if (limited) {
    out->status = GTEXT_ENCODING_E_LIMIT_OUTPUT;
    return false;
  }
  return true;
}

/* ---- the buffer form ----------------------------------------------------------------------- */

_Static_assert(sizeof(GTEXT_Allocator) <= GTEXT_ENCODING_BUF_HEADER, "the allocator fits in the buffer's header");

/// Copy the options' allocator (or the default) into `out`.
void gtext_encoding_allocator_copy(GTEXT_Allocator * out, const GTEXT_Allocator * given) {
  *out = *(given ? given : gtext_allocator_default());
}

/// The sink function that appends to a GTEXT_Encoding_Buffer, growing it by doubling; false (with `oom` set) if it cannot.
bool gtext_encoding_buffer_write(void * user, const char * bytes, size_t len) {
  GTEXT_Encoding_Buffer * b = user;
  if (len > SIZE_MAX - b->length - 1 - GTEXT_ENCODING_BUF_HEADER) {
    b->oom = true;
    return false;
  }
  if (b->length + len + 1 > b->capacity) {
    size_t cap = b->capacity ? b->capacity : 256;
    while (cap < b->length + len + 1) {
      if (cap > (SIZE_MAX - GTEXT_ENCODING_BUF_HEADER) / 2) {
        b->oom = true;
        return false;
      }
      cap *= 2;
    }
    unsigned char * grown = gtext_allocator_realloc(&b->alloc, b->base, GTEXT_ENCODING_BUF_HEADER + cap);
    if (!grown) {
      b->oom = true;
      return false;
    }
    if (!b->base) {
      memcpy(grown, &b->alloc, sizeof(GTEXT_Allocator));
    }
    b->base = grown;
    b->capacity = cap;
  }
  memcpy(b->base + GTEXT_ENCODING_BUF_HEADER + b->length, bytes, len);
  b->length += len;
  b->base[GTEXT_ENCODING_BUF_HEADER + b->length] = '\0';
  return true;
}

void gtext_encoding_buffer_free(char * buffer) {
  if (!buffer) {
    return;
  }
  unsigned char * base = (unsigned char *)buffer - GTEXT_ENCODING_BUF_HEADER;
  GTEXT_Allocator alloc;
  memcpy(&alloc, base, sizeof(alloc));
  gtext_allocator_free(&alloc, base);
}
