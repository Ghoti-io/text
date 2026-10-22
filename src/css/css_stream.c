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
 * The incremental parser (css_stream.h). CSS cannot be parsed before its end:
 * the tokenizer matches brackets over the whole input and a rule's grammar is
 * known only when its block closes. So a stream keeps the input, bounded by
 * `max_input_bytes`, and parses once, at the end; what it adds is everything
 * that must not depend on the cuts:
 *
 *  - the encoding is decided from the first kilobyte whatever pieces it came
 *    in, and only then is a decoder made (so bytes that arrive before the
 *    decision are kept, at most the window and the piece that completed it);
 *  - UTF-8 is kept as bytes and read in place at the end, like the one-call
 *    function reads it;
 *  - any other encoding goes through the Encoding module's decoder as the
 *    pieces come, which is stateful across cuts, into text and a map of where
 *    each code point came from (css_decode.c), so only the decoded text is
 *    kept;
 *  - the limit on the input is checked as each piece arrives.
 *
 * Both buffers grow by doubling, so no work is proportional to the number of
 * calls. The parse at the end is the one-call function's (css_parse_decoded),
 * so the tree, the errors and the verdict are those of the whole input by
 * construction; the tests check that the rest is too.
 */

#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "css_internal.h"

/// Where in its life a stream is.
typedef enum {
  PH_SNIFF,   ///< Bytes mode, fewer than a window of bytes, no decision yet.
  PH_RAW,     ///< The bytes are kept as they are: text, or bytes that are UTF-8.
  PH_DECODING ///< The bytes go through a decoder.
} phase;

struct GTEXT_CSS_Stream {
  GTEXT_Allocator alloc;            ///< Everything is allocated through this.
  GTEXT_CSS_Tree_Kind kind;         ///< Which tree.
  GTEXT_CSS_Parse_Options opts;     ///< Copied; its allocator and location point into this struct.
  char * location;                  ///< The copy of opts.location, or NULL.
  bool bytes_mode;                  ///< False: the input is text.
  GTEXT_CSS_Bytes_Options bopts;    ///< Copied; its label points to `label`.
  char * label;                     ///< The copy of the protocol label, or NULL.
  size_t max_input;                 ///< The limit on the bytes fed.
  size_t fed;                       ///< Bytes fed (up to and including the piece that went past the limit).
  phase ph;                         ///< Where it is.
  unsigned char * raw;              ///< PH_SNIFF and PH_RAW: the bytes.
  size_t raw_len;                   ///< Their number.
  size_t raw_cap;                   ///< Allocated.
  GTEXT_CSS_Encoding_Decision dec;  ///< The decision, once made.
  css_decode d;                     ///< PH_DECODING: the decoder, text and map.
  bool d_open;                      ///< `d` has been begun.
  GTEXT_CSS_Status failed;          ///< OK, or the failure every call reports.
  bool finished;                    ///< gtext_css_stream_finish() has been called.
  size_t work;                      ///< Units of work (css_ctx::work).
};

/* The bytes beyond the limit that are still read, to place a limit failure in
   text exactly as the one-call function does: a position inside a multi-byte
   sequence is the end of that sequence, so up to three bytes past the limit. */
#define TAIL_BYTES 3

static void stream_fail(GTEXT_CSS_Stream * s, GTEXT_CSS_Status status) {
  if (s->failed == GTEXT_CSS_OK) {
    s->failed = status;
  }
}

static bool raw_reserve(GTEXT_CSS_Stream * s, size_t more) {
  if (more > SIZE_MAX - s->raw_len) {
    stream_fail(s, GTEXT_CSS_E_OOM);
    return false;
  }
  if (s->raw_cap - s->raw_len >= more) {
    return true;
  }
  size_t cap = s->raw_cap ? s->raw_cap : 4096;
  while (cap - s->raw_len < more) {
    if (cap > SIZE_MAX / 2) {
      cap = s->raw_len + more;
      break;
    }
    cap *= 2;
  }
  unsigned char * grown = gtext_allocator_realloc(&s->alloc, s->raw, cap);
  if (!grown) {
    stream_fail(s, GTEXT_CSS_E_OOM);
    return false;
  }
  s->raw = grown;
  s->raw_cap = cap;
  s->work += s->raw_len; /* the copy a growth may make */
  return true;
}

static bool raw_append(GTEXT_CSS_Stream * s, const unsigned char * data, size_t n) {
  if (n == 0) {
    return true;
  }
  if (!raw_reserve(s, n)) {
    return false;
  }
  memcpy(s->raw + s->raw_len, data, n);
  s->raw_len += n;
  s->work += n;
  return true;
}

static char * copy_string(const GTEXT_Allocator * a, const char * text) {
  const size_t n = strlen(text);
  char * p = gtext_allocator_malloc(a, n + 1);
  if (p) {
    memcpy(p, text, n + 1);
  }
  return p;
}

static GTEXT_CSS_Status stream_make(GTEXT_CSS_Tree_Kind kind, const GTEXT_CSS_Bytes_Options * bopts,
    bool bytes_mode, const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Stream ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  if ((unsigned)kind > (unsigned)GTEXT_CSS_TREE_COMMA_LIST) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator alloc = *(options && options->allocator ? options->allocator : gtext_allocator_default());
  GTEXT_CSS_Stream * s = gtext_allocator_malloc(&alloc, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  memset(s, 0, sizeof *s);
  s->alloc = alloc;
  s->kind = kind;
  if (options) {
    s->opts = *options;
  }
  s->opts.allocator = &s->alloc;
  s->max_input = css_max_input(options);
  s->bytes_mode = bytes_mode;
  s->ph = bytes_mode ? PH_SNIFF : PH_RAW;
  if (options && options->location) {
    s->location = copy_string(&alloc, options->location);
    if (!s->location) {
      gtext_css_stream_free(s);
      return GTEXT_CSS_E_OOM;
    }
    s->opts.location = s->location;
  }
  if (bopts) {
    s->bopts = *bopts;
    if (bopts->protocol_label) {
      s->label = copy_string(&alloc, bopts->protocol_label);
      if (!s->label) {
        gtext_css_stream_free(s);
        return GTEXT_CSS_E_OOM;
      }
      s->bopts.protocol_label = s->label;
    }
  }
  *out = s;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_stream_new(GTEXT_CSS_Tree_Kind kind, const GTEXT_CSS_Parse_Options * options,
    GTEXT_CSS_Stream ** out) {
  return stream_make(kind, NULL, false, options, out);
}

GTEXT_CSS_Status gtext_css_stream_new_bytes(GTEXT_CSS_Tree_Kind kind, const GTEXT_CSS_Bytes_Options * bytes_options,
    const GTEXT_CSS_Parse_Options * options, GTEXT_CSS_Stream ** out) {
  return stream_make(kind, bytes_options, true, options, out);
}

void gtext_css_stream_free(GTEXT_CSS_Stream * s) {
  if (!s) {
    return;
  }
  const GTEXT_Allocator alloc = s->alloc;
  if (s->d_open) {
    css_decode_free(&s->d);
  }
  gtext_allocator_free(&alloc, s->raw);
  gtext_allocator_free(&alloc, s->label);
  gtext_allocator_free(&alloc, s->location);
  gtext_allocator_free(&alloc, s);
}

/* The window is here (or the input has ended): decide the encoding, and from
   here on the bytes go where it says. */
static bool decide(GTEXT_CSS_Stream * s) {
  if (gtext_css_determine_encoding(s->raw, s->raw_len, &s->bopts, &s->dec) != GTEXT_CSS_OK) {
    stream_fail(s, GTEXT_CSS_E_INTERNAL);
    return false;
  }
  if (s->dec.encoding == GTEXT_ENCODING_UTF_8) {
    s->ph = PH_RAW;
    return true;
  }
  s->d_open = true;
  if (!css_decode_begin(&s->d, &s->alloc, s->dec.encoding)) {
    stream_fail(s, s->d.status);
    return false;
  }
  s->ph = PH_DECODING;
  const bool ok = css_decode_feed(&s->d, s->raw, s->raw_len, false);
  gtext_allocator_free(&s->alloc, s->raw);
  s->raw = NULL;
  s->raw_len = s->raw_cap = 0;
  if (!ok) {
    stream_fail(s, s->d.status);
    return false;
  }
  return true;
}

GTEXT_CSS_Status gtext_css_stream_feed(GTEXT_CSS_Stream * s, const void * data, size_t length) {
  if (!s) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!data && length > 0) {
    return GTEXT_CSS_E_INVALID;
  }
  s->work++; /* a call costs a constant */
  const unsigned char * bytes = data;
  if (s->failed != GTEXT_CSS_OK) {
    /* A text stream that went past its limit still reads the few bytes after it, to place the failure. */
    if (s->failed == GTEXT_CSS_E_LIMIT_INPUT && !s->bytes_mode && !s->finished && s->raw_len < s->max_input + TAIL_BYTES) {
      size_t room = s->max_input + TAIL_BYTES - s->raw_len;
      (void)raw_append(s, bytes, length < room ? length : room);
    }
    return s->failed;
  }
  if (s->finished) {
    return GTEXT_CSS_E_INVALID;
  }
  if (length == 0) {
    return GTEXT_CSS_OK;
  }
  if (length > s->max_input - s->fed) {
    s->fed += length < SIZE_MAX - s->fed ? length : SIZE_MAX - s->fed;
    stream_fail(s, GTEXT_CSS_E_LIMIT_INPUT);
    if (!s->bytes_mode) {
      const size_t kept = s->max_input - (s->raw_len < s->max_input ? s->raw_len : s->max_input);
      const size_t take = kept + TAIL_BYTES < length ? kept + TAIL_BYTES : length;
      (void)raw_append(s, bytes, take);
      if (s->failed != GTEXT_CSS_E_LIMIT_INPUT) { /* the append ran out of memory first */
        return s->failed;
      }
    }
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  s->fed += length;
  if (s->ph == PH_DECODING) {
    if (!css_decode_feed(&s->d, bytes, length, false)) {
      stream_fail(s, s->d.status);
      return s->failed;
    }
    return GTEXT_CSS_OK;
  }
  if (!raw_append(s, bytes, length)) {
    return s->failed;
  }
  if (s->ph == PH_SNIFF && s->raw_len >= GTEXT_CSS_ENCODING_WINDOW) {
    if (!decide(s)) {
      return s->failed;
    }
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status finish(GTEXT_CSS_Stream * s, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result,
    size_t * work) {
  if (work) {
    *work = 0;
  }
  if (result) {
    memset(result, 0, sizeof *result);
  }
  if (!out) {
    if (result) {
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  if (s->finished && s->failed == GTEXT_CSS_OK) {
    if (result) {
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  s->finished = true;
  if (s->failed == GTEXT_CSS_OK && s->ph == PH_SNIFF) {
    (void)decide(s); /* the input is all here, and shorter than the window */
  }
  if (s->failed == GTEXT_CSS_OK && s->ph == PH_DECODING) {
    if (!css_decode_feed(&s->d, NULL, 0, true)) {
      stream_fail(s, s->d.status);
    }
  }
  if (s->failed != GTEXT_CSS_OK) {
    size_t line = 0, column = 0;
    if (s->failed == GTEXT_CSS_E_LIMIT_INPUT && !s->bytes_mode) {
      css_ctx ctx;
      css_ctx_init(&ctx, &s->opts, false);
      ctx.in = s->raw;
      ctx.in_len = s->raw_len;
      css_position_of(&ctx, s->max_input, &line, &column);
      css_ctx_free(&ctx, false);
    }
    css_refused_result(&s->opts, s->failed, s->max_input, line, column, s->fed, NULL, result);
    if (work) {
      *work = s->work;
    }
    return s->failed;
  }
  css_srcinfo info;
  memset(&info, 0, sizeof info);
  info.source_length = s->fed;
  css_srcmap shift;
  memset(&shift, 0, sizeof shift);
  size_t parse_work = 0;
  GTEXT_CSS_Status st;
  if (!s->bytes_mode) {
    st = css_parse_decoded(s->kind, s->raw ? (const char *)s->raw : "", s->raw_len, &s->opts, NULL, out, result,
        &parse_work);
  }
  else if (s->ph == PH_RAW) {
    info.encoding.encoding = s->dec.encoding;
    info.encoding.source = s->dec.source;
    info.encoding.bom_length = s->dec.bom_length;
    shift.shift = s->dec.bom_length;
    shift.dec_total = s->raw_len - s->dec.bom_length;
    shift.src_total = s->raw_len;
    info.map = &shift;
    st = css_parse_decoded(s->kind, s->raw ? (const char *)s->raw + s->dec.bom_length : "",
        s->raw_len - s->dec.bom_length, &s->opts, &info, out, result, &parse_work);
  }
  else {
    info.encoding.encoding = s->dec.encoding;
    info.encoding.source = s->dec.source;
    info.encoding.bom_length = s->dec.bom_length;
    info.map = &s->d.map;
    st = css_parse_decoded(s->kind, s->d.text ? s->d.text : "", s->d.len, &s->opts, &info, out, result, &parse_work);
  }
  if (work) {
    *work = s->work + parse_work + (s->d_open ? s->d.work : 0);
  }
  return st;
}

GTEXT_CSS_Status gtext_css_stream_finish(GTEXT_CSS_Stream * s, GTEXT_CSS_Tree ** out, GTEXT_CSS_Result * result) {
  if (!s) {
    if (result) {
      memset(result, 0, sizeof *result);
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  return finish(s, out, result, NULL);
}

/** The tests' view of a stream's memory (css_internal.h). */
size_t gtext_css_stream_kept_bytes(const GTEXT_CSS_Stream * s, size_t * allocated) {
  if (!s) {
    return 0;
  }
  if (allocated) {
    *allocated = s->raw_cap + (s->d_open ? s->d.cap + s->d.map.cap * sizeof(css_srcrun) : 0);
  }
  return s->raw_len + (s->d_open ? s->d.len : 0);
}

/** The quadratic suite's view of a stream (css_internal.h). */
GTEXT_CSS_Status gtext_css_stream_work(GTEXT_CSS_Tree_Kind kind, const void * data, size_t length, size_t chunk,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options, size_t * work) {
  GTEXT_CSS_Stream * s = NULL;
  GTEXT_CSS_Status st = bytes_options ? gtext_css_stream_new_bytes(kind, bytes_options, options, &s)
                                      : gtext_css_stream_new(kind, options, &s);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const unsigned char * p = data;
  size_t at = 0;
  const size_t step = chunk ? chunk : (length ? length : 1);
  while (at < length && st == GTEXT_CSS_OK) {
    const size_t n = length - at < step ? length - at : step;
    st = gtext_css_stream_feed(s, p + at, n);
    at += n;
  }
  GTEXT_CSS_Tree * tree = NULL;
  size_t w = 0;
  const GTEXT_CSS_Status fin = finish(s, &tree, NULL, &w);
  if (st == GTEXT_CSS_OK) {
    st = fin;
  }
  gtext_css_tree_free(fin == GTEXT_CSS_OK ? tree : NULL);
  gtext_css_stream_free(s);
  if (work) {
    *work = w;
  }
  return st;
}
