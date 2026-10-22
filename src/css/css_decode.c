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
 * A stylesheet's bytes: CSS Syntax 3.2, "determine the fallback encoding" and
 * the Encoding Standard's "decode" behind it, through the Encoding module so
 * that every offset the parse reports is in the caller's bytes (the pattern
 * the HTML module follows: the decoder's source map says where each code point
 * came from).
 *
 * UTF-8 is not decoded. The tokenizer reads it in place, and the map from its
 * text to the bytes is one number, the length of the byte order mark. Every
 * other encoding is decoded into a copy, and the map is a list of runs
 * (css_srcrun) that is as short as the pattern of the text allows.
 *
 * The decoding is one piece of code for the one-call function and the stream
 * (css_stream.c): the stream feeds the same functions the pieces, so that the
 * only difference between them is where the input was cut.
 *
 * Reference: CSS Syntax Module Level 3, Editor's Draft of 20 October 2026,
 * section 3.2, https://drafts.csswg.org/css-syntax/#input-byte-stream
 * (retrieved 2026-10-21).
 */

#include <ghoti.io/text/encoding/encoding_core.h>
#include <ghoti.io/text/encoding/encoding_decode.h>
#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "css_internal.h"

/* ---- options and the encoding ------------------------------------------ */

GTEXT_CSS_Bytes_Options gtext_css_bytes_options_default(void) {
  GTEXT_CSS_Bytes_Options o;
  memset(&o, 0, sizeof o);
  return o;
}

/* A byte of the label in "@charset "XX*";": 01 to 21 and 23 to 7F, so neither
   NUL nor the quote that ends it. */
static bool is_label_byte(unsigned char c) {
  return (c >= 0x01 && c <= 0x21) || (c >= 0x23 && c <= 0x7F);
}

/* The label of a leading @charset declaration, byte for byte, or NULL: the
   bytes `@charset "`, the label, and `";`, all inside the first 1024. */
static bool charset_label(const unsigned char * b, size_t window, size_t * begin, size_t * end) {
  static const char prefix[] = "@charset \"";
  const size_t plen = sizeof prefix - 1;
  if (window < plen || memcmp(b, prefix, plen) != 0) {
    return false;
  }
  size_t i = plen;
  while (i < window && is_label_byte(b[i])) {
    i++;
  }
  if (i + 1 < window && b[i] == '"' && b[i + 1] == ';') {
    *begin = plen;
    *end = i;
    return true;
  }
  return false;
}

/** Decide the encoding of a stylesheet's bytes (css_decode.h). */
GTEXT_CSS_Status gtext_css_determine_encoding(const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * options, GTEXT_CSS_Encoding_Decision * out) {
  if (!out || (!bytes && length > 0)) {
    return GTEXT_CSS_E_INVALID;
  }
  const unsigned char * b = bytes;
  GTEXT_CSS_Encoding_Decision d;
  memset(&d, 0, sizeof d);
  /* "Decode": a byte order mark wins over every label. */
  if (length >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
    d.encoding = GTEXT_ENCODING_UTF_8;
    d.source = GTEXT_CSS_ENCODING_SOURCE_BOM;
    d.bom_length = 3;
    *out = d;
    return GTEXT_CSS_OK;
  }
  if (length >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
    d.encoding = GTEXT_ENCODING_UTF_16BE;
    d.source = GTEXT_CSS_ENCODING_SOURCE_BOM;
    d.bom_length = 2;
    *out = d;
    return GTEXT_CSS_OK;
  }
  if (length >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
    d.encoding = GTEXT_ENCODING_UTF_16LE;
    d.source = GTEXT_CSS_ENCODING_SOURCE_BOM;
    d.bom_length = 2;
    *out = d;
    return GTEXT_CSS_OK;
  }
  /* "If HTTP or equivalent protocol provides an encoding label ... get an encoding from encoding label. If that
     does not return failure, return it." */
  if (options && options->protocol_label) {
    const GTEXT_Encoding e = gtext_encoding_for_label(options->protocol_label, strlen(options->protocol_label));
    if (e != GTEXT_ENCODING_NONE) {
      d.encoding = e;
      d.source = GTEXT_CSS_ENCODING_SOURCE_PROTOCOL;
      *out = d;
      return GTEXT_CSS_OK;
    }
  }
  /* "Otherwise, check stylesheet's byte stream. If the first 1024 bytes of the stream begin with the hex sequence
     40 63 68 61 72 73 65 74 20 22 XX* 22 3B ..." */
  const size_t window = length < GTEXT_CSS_ENCODING_WINDOW ? length : GTEXT_CSS_ENCODING_WINDOW;
  size_t lb = 0, le = 0;
  if (charset_label(b, window, &lb, &le)) {
    GTEXT_Encoding e = gtext_encoding_for_label((const char *)b + lb, le - lb);
    if (e == GTEXT_ENCODING_UTF_16BE || e == GTEXT_ENCODING_UTF_16LE) {
      e = GTEXT_ENCODING_UTF_8; /* the declaration is ASCII, so the bytes are not UTF-16 */
    }
    if (e != GTEXT_ENCODING_NONE) {
      d.encoding = e;
      d.source = GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE;
      *out = d;
      return GTEXT_CSS_OK;
    }
  }
  /* "Otherwise, if an environment encoding is provided by the referring document, return it." */
  if (options && options->environment_encoding != GTEXT_ENCODING_NONE
      && (unsigned)options->environment_encoding < (unsigned)GTEXT_ENCODING_COUNT) {
    d.encoding = options->environment_encoding;
    d.source = GTEXT_CSS_ENCODING_SOURCE_ENVIRONMENT;
    *out = d;
    return GTEXT_CSS_OK;
  }
  /* "Otherwise, return utf-8." */
  d.encoding = GTEXT_ENCODING_UTF_8;
  d.source = GTEXT_CSS_ENCODING_SOURCE_DEFAULT;
  *out = d;
  return GTEXT_CSS_OK;
}

/* ---- the decoder and the map ------------------------------------------- */

/* One more code point of the text: `dlen` bytes of it, read from `width`
   bytes of the source at `src`. It extends the last run while the pattern
   holds (the same lengths, the source contiguous). */
static bool map_add(css_decode * d, size_t dec, size_t src, uint8_t dlen, uint8_t width) {
  css_srcmap * m = &d->map;
  if (m->nruns > 0) {
    css_srcrun * last = &m->runs[m->nruns - 1];
    if (last->dlen == dlen && last->width == width && last->count < UINT32_MAX
        && last->src + (size_t)last->count * width == src) {
      last->count++;
      return true;
    }
  }
  if (m->nruns == m->cap) {
    const size_t cap = m->cap ? m->cap * 2 : 64;
    if (cap > SIZE_MAX / sizeof *m->runs) {
      d->status = GTEXT_CSS_E_OOM;
      return false;
    }
    css_srcrun * grown = gtext_allocator_realloc(&d->alloc, m->runs, cap * sizeof *m->runs);
    if (!grown) {
      d->status = GTEXT_CSS_E_OOM;
      return false;
    }
    m->runs = grown;
    m->cap = cap;
    d->work += m->nruns; /* the copy a growth may make */
  }
  css_srcrun * r = &m->runs[m->nruns++];
  r->dec = dec;
  r->src = src;
  r->count = 1;
  r->dlen = dlen;
  r->width = width;
  return true;
}

static bool text_reserve(css_decode * d, size_t more) {
  if (more > SIZE_MAX - d->len) {
    d->status = GTEXT_CSS_E_OOM;
    return false;
  }
  if (d->cap - d->len >= more) {
    return true;
  }
  size_t cap = d->cap ? d->cap : 4096;
  while (cap - d->len < more) {
    if (cap > SIZE_MAX / 2) {
      cap = d->len + more;
      break;
    }
    cap *= 2;
  }
  char * grown = gtext_allocator_realloc(&d->alloc, d->text, cap);
  if (!grown) {
    d->status = GTEXT_CSS_E_OOM;
    return false;
  }
  d->text = grown;
  d->cap = cap;
  d->work += d->len;
  return true;
}

/* The Encoding module's mapped sink: a chunk of UTF-8, and for each byte of it
   where its code point was read (all the bytes of a code point say the same). */
static bool decode_sink(void * user, const char * bytes, size_t len, const GTEXT_Encoding_Source_Map * map) {
  css_decode * d = user;
  if (!text_reserve(d, len)) {
    return false;
  }
  const size_t base = d->len;
  memcpy(d->text + base, bytes, len);
  d->work += len;
  size_t i = 0;
  while (i < len) {
    const unsigned char c = (unsigned char)bytes[i];
    const size_t dlen = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (dlen == 0 || i + dlen > len) { /* the decoder writes whole code points (and a width of 0 is possible: an ISO-2022-JP escape that is not one writes its U+FFFD before the byte after it is read) */
      d->status = GTEXT_CSS_E_INTERNAL;
      return false;
    }
    if (!map_add(d, base + i, map->offsets[i], (uint8_t)dlen, map->widths[i])) {
      return false;
    }
    i += dlen;
  }
  d->len += len;
  return true;
}

/* The index that makes a lookup cost a constant: for each block of the text, the last run that starts at or before the
   block's first byte. Made when the decoding has ended and the length is known. */
static bool map_index(css_decode * d) {
  css_srcmap * m = &d->map;
  gtext_allocator_free(&d->alloc, m->blocks);
  m->blocks = NULL;
  m->nblocks = 0;
  if (m->nruns == 0) {
    return true;
  }
  const size_t n = m->dec_total / CSS_SRCMAP_BLOCK + 1;
  if (n > SIZE_MAX / sizeof *m->blocks) {
    d->status = GTEXT_CSS_E_OOM;
    return false;
  }
  m->blocks = gtext_allocator_malloc(&d->alloc, n * sizeof *m->blocks);
  if (!m->blocks) {
    d->status = GTEXT_CSS_E_OOM;
    return false;
  }
  size_t r = 0;
  for (size_t b = 0; b < n; b++) {
    while (r + 1 < m->nruns && m->runs[r + 1].dec <= b * CSS_SRCMAP_BLOCK) {
      r++;
    }
    m->blocks[b] = r;
  }
  m->nblocks = n;
  d->work += n + m->nruns;
  return true;
}

/** Begin decoding (css_internal.h). */
bool css_decode_begin(css_decode * d, const GTEXT_Allocator * alloc, GTEXT_Encoding encoding) {
  memset(d, 0, sizeof *d);
  d->alloc = *(alloc ? alloc : gtext_allocator_default());
  d->status = GTEXT_CSS_OK;
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.allocator = &d->alloc;
  o.bom_sniffing = true; /* "decode": a byte order mark is not part of the text */
  o.track_offsets = true;
  o.max_output_bytes = SIZE_MAX; /* the limit is on the bytes given, and is the caller's */
  const GTEXT_Encoding_Status s = gtext_decoder_new(encoding, &o, &d->decoder);
  if (s != GTEXT_ENCODING_OK) {
    d->status = s == GTEXT_ENCODING_E_OOM ? GTEXT_CSS_E_OOM : GTEXT_CSS_E_INTERNAL;
    return false;
  }
  return true;
}

/** Decode more bytes (css_internal.h). */
bool css_decode_feed(css_decode * d, const unsigned char * bytes, size_t n, bool final) {
  if (d->status != GTEXT_CSS_OK) {
    return false;
  }
  if (n == 0 && !final) {
    return true;
  }
  const GTEXT_Encoding_Mapped_Sink sink = {decode_sink, d};
  GTEXT_Decoder_Result r;
  memset(&r, 0, sizeof r);
  const GTEXT_Encoding_Status s = gtext_decoder_decode_mapped(d->decoder, bytes ? bytes : (const unsigned char *)"", n,
      final, &sink, &r);
  d->work += n + r.steps;
  d->src_fed += n;
  if (s == GTEXT_ENCODING_E_WRITE) {
    if (d->status == GTEXT_CSS_OK) {
      d->status = GTEXT_CSS_E_INTERNAL;
    }
    return false;
  }
  if (s != GTEXT_ENCODING_OK) {
    d->status = s == GTEXT_ENCODING_E_OOM ? GTEXT_CSS_E_OOM : GTEXT_CSS_E_INTERNAL;
    return false;
  }
  d->map.dec_total = d->len;
  d->map.src_total = d->src_fed;
  if (final && !map_index(d)) {
    return false;
  }
  return true;
}

/** Free what the decoder holds (css_internal.h). */
void css_decode_free(css_decode * d) {
  gtext_decoder_free(d->decoder);
  d->decoder = NULL;
  gtext_allocator_free(&d->alloc, d->text);
  d->text = NULL;
  gtext_allocator_free(&d->alloc, d->map.runs);
  d->map.runs = NULL;
  gtext_allocator_free(&d->alloc, d->map.blocks);
  d->map.blocks = NULL;
  d->map.nblocks = 0;
  d->len = d->cap = 0;
  d->map.nruns = d->map.cap = 0;
}

/* ---- results of a byte parse that did not get to parse -------------------- */

/** A result for a refusal before any text existed (css_internal.h). */
void css_refused_result(const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Status status, size_t at,
    size_t line, size_t column, size_t input_bytes, const css_enc_info * encoding,
    GTEXT_CSS_Result * result) {
  if (!result) {
    return;
  }
  css_ctx ctx;
  css_ctx_init(&ctx, options, true);
  if (status == GTEXT_CSS_E_OOM) {
    css_fail(&ctx, status);
  }
  else {
    css_fail_at(&ctx, status, at, line, column);
  }
  (void)css_finish_result(&ctx, result, input_bytes);
  if (encoding) {
    result->encoding = encoding->encoding;
    result->encoding_source = encoding->source;
    result->bom_length = encoding->bom_length;
  }
  css_ctx_free(&ctx, false);
}

/* ---- the one-call function --------------------------------------------- */

/// The one-call parse of a stylesheet's bytes (also the quadratic suite's, with `work`).
static GTEXT_CSS_Status parse_bytes(GTEXT_CSS_Tree_Kind kind, const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * bopts, const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Tree ** out,
    GTEXT_CSS_Result * result, size_t * work) {
  if (work) {
    *work = 0;
  }
  if (result) {
    memset(result, 0, sizeof *result);
  }
  if (!out || (!bytes && length > 0)) {
    if (result) {
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  const size_t max_input = css_max_input(options);
  if (length > max_input) {
    css_refused_result(options, GTEXT_CSS_E_LIMIT_INPUT, max_input, 0, 0, length, NULL, result);
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  GTEXT_CSS_Encoding_Decision dec;
  if (gtext_css_determine_encoding(bytes, length, bopts, &dec) != GTEXT_CSS_OK) {
    return GTEXT_CSS_E_INTERNAL;
  }
  css_srcinfo info;
  memset(&info, 0, sizeof info);
  info.source_length = length;
  info.encoding.encoding = dec.encoding;
  info.encoding.source = dec.source;
  info.encoding.bom_length = dec.bom_length;
  css_srcmap shift;
  memset(&shift, 0, sizeof shift);
  if (dec.encoding == GTEXT_ENCODING_UTF_8) {
    /* Read in place: the text is the bytes after the byte order mark. */
    shift.shift = dec.bom_length;
    shift.dec_total = length - dec.bom_length;
    shift.src_total = length;
    info.map = &shift;
    const char * text = bytes ? (const char *)bytes + dec.bom_length : "";
    return css_parse_decoded(kind, text, length - dec.bom_length, options, &info, out, result, work);
  }
  css_decode d;
  const GTEXT_Allocator * alloc = options ? options->allocator : NULL;
  if (!css_decode_begin(&d, alloc, dec.encoding) || !css_decode_feed(&d, bytes, length, true)) {
    const GTEXT_CSS_Status st = d.status;
    css_decode_free(&d);
    css_refused_result(options, st, 0, 0, 0, length, &info.encoding, result);
    return st;
  }
  info.map = &d.map;
  size_t parse_work = 0;
  const GTEXT_CSS_Status st = css_parse_decoded(kind, d.text ? d.text : "", d.len, options, &info, out, result,
      &parse_work);
  if (work) {
    *work = parse_work + d.work;
  }
  css_decode_free(&d);
  return st;
}

/** Parse a stylesheet from bytes (css_decode.h). */
GTEXT_CSS_Status gtext_css_parse_stylesheet_bytes(const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  return parse_bytes(GTEXT_CSS_TREE_STYLESHEET, bytes, length, bytes_options, options, out, result, NULL);
}

/** The quadratic suite's view of a parse of bytes (css_internal.h). */
GTEXT_CSS_Status gtext_css_parse_bytes_work(GTEXT_CSS_Tree_Kind kind, const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options, size_t * work) {
  GTEXT_CSS_Tree * tree = NULL;
  size_t w = 0;
  const GTEXT_CSS_Status st = parse_bytes(kind, bytes, length, bytes_options, options, &tree, NULL, &w);
  gtext_css_tree_free(st == GTEXT_CSS_OK ? tree : NULL);
  if (work) {
    *work = w;
  }
  return st;
}
