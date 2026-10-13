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
 * The decoders: the standard's handlers for UTF-8, UTF-16BE/LE, `replacement`,
 * `x-user-defined` and the single-byte encodings, and the driver that runs any
 * handler over a stream (chunking, the two error modes, the byte order mark
 * options, the output limit).
 *
 * The handlers are the standard's algorithms transcribed: each step of section
 * 8.1.1, 9.1, 14.1.1, 14.2.1 and 14.5.1 is a line of code, in the standard's order,
 * with the standard's names for the state. The driver is "process an item" and
 * "process a queue" (section 4.1) made incremental.
 */

#include <ghoti.io/text/encoding.h>

#include "encoding_internal.h"

#include <stdint.h>
#include <string.h>

/* ---- UTF-8 (section 8.1.1) ------------------------------------------------------------- */

/// Start UTF-8's decoder: no sequence in progress and the boundaries of a continuation byte at their widest.
void gtext_utf8_init(GTEXT_Dec_State * state, GTEXT_Encoding encoding) {
  (void)encoding;
  state->utf8.code_point = 0;
  state->utf8.bytes_seen = 0;
  state->utf8.bytes_needed = 0;
  state->utf8.lower = 0x80;
  state->utf8.upper = 0xBF;
}

/// Is the decoder between characters?
bool gtext_utf8_idle(const GTEXT_Dec_State * state) {
  return state->utf8.bytes_needed == 0;
}

/// UTF-8's decoder handler, step for step as section 8.1.1 writes it.
GTEXT_Dec_Result gtext_utf8_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  GTEXT_Utf8_State * s = &state->utf8;
  /* If byte is end-of-queue and UTF-8 bytes needed is not 0, set it to 0 and return error. */
  if (byte < 0 && s->bytes_needed != 0) {
    s->bytes_needed = 0;
    return GTEXT_DEC_ERROR;
  }
  /* If byte is end-of-queue, return finished. */
  if (byte < 0) {
    return GTEXT_DEC_FINISHED;
  }
  /* If UTF-8 bytes needed is 0, based on byte: */
  if (s->bytes_needed == 0) {
    if (byte <= 0x7F) {
      out->cp[0] = (uint32_t)byte;
      out->n = 1;
      return GTEXT_DEC_EMIT;
    }
    if (byte >= 0xC2 && byte <= 0xDF) {
      s->bytes_needed = 1;
      s->code_point = (uint32_t)byte & 0x1F;
    } else if (byte >= 0xE0 && byte <= 0xEF) {
      if (byte == 0xE0) {
        s->lower = 0xA0;
      }
      if (byte == 0xED) {
        s->upper = 0x9F;
      }
      s->bytes_needed = 2;
      s->code_point = (uint32_t)byte & 0xF;
    } else if (byte >= 0xF0 && byte <= 0xF4) {
      if (byte == 0xF0) {
        s->lower = 0x90;
      }
      if (byte == 0xF4) {
        s->upper = 0x8F;
      }
      s->bytes_needed = 3;
      s->code_point = (uint32_t)byte & 0x7;
    } else {
      return GTEXT_DEC_ERROR;
    }
    return GTEXT_DEC_CONTINUE;
  }
  /* If byte is not in the range UTF-8 lower boundary to UTF-8 upper boundary, inclusive: */
  if (byte < s->lower || byte > s->upper) {
    s->code_point = 0;
    s->bytes_needed = 0;
    s->bytes_seen = 0;
    s->lower = 0x80;
    s->upper = 0xBF;
    out->restore_n = 1; /* Restore byte to ioQueue. */
    out->restore[0] = (uint8_t)byte;
    return GTEXT_DEC_ERROR;
  }
  s->lower = 0x80;
  s->upper = 0xBF;
  s->code_point = (s->code_point << 6) | ((uint32_t)byte & 0x3F);
  s->bytes_seen++;
  if (s->bytes_seen != s->bytes_needed) {
    return GTEXT_DEC_CONTINUE;
  }
  out->cp[0] = s->code_point;
  out->n = 1;
  s->code_point = 0;
  s->bytes_needed = 0;
  s->bytes_seen = 0;
  return GTEXT_DEC_EMIT;
}

/* ---- the shared UTF-16 decoder (section 14.2.1) --------------------------------------- */

static void utf16_init(GTEXT_Dec_State * state, GTEXT_Encoding encoding) {
  state->utf16.lead_byte = -1;
  state->utf16.lead_surrogate = 0;
  state->utf16.big_endian = encoding == GTEXT_ENCODING_UTF_16BE;
}

static GTEXT_Dec_Result utf16_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  GTEXT_Utf16_State * s = &state->utf16;
  if (byte < 0) {
    if (s->lead_byte >= 0 || s->lead_surrogate != 0) {
      s->lead_byte = -1;
      s->lead_surrogate = 0;
      return GTEXT_DEC_ERROR;
    }
    return GTEXT_DEC_FINISHED;
  }
  if (s->lead_byte < 0) {
    s->lead_byte = byte;
    return GTEXT_DEC_CONTINUE;
  }
  const uint32_t code_unit = s->big_endian ? (((uint32_t)s->lead_byte << 8) + (uint32_t)byte)
                                           : (((uint32_t)byte << 8) + (uint32_t)s->lead_byte);
  s->lead_byte = -1;
  if (s->lead_surrogate != 0) {
    const uint32_t leading = s->lead_surrogate;
    s->lead_surrogate = 0;
    if (code_unit >= 0xDC00 && code_unit <= 0xDFFF) {
      out->cp[0] = 0x10000 + ((leading - 0xD800) << 10) + (code_unit - 0xDC00);
      out->n = 1;
      return GTEXT_DEC_EMIT;
    }
    const uint8_t byte1 = (uint8_t)(code_unit >> 8);
    const uint8_t byte2 = (uint8_t)(code_unit & 0xFF);
    out->restore_n = 2;
    out->restore[0] = s->big_endian ? byte1 : byte2;
    out->restore[1] = s->big_endian ? byte2 : byte1;
    return GTEXT_DEC_ERROR;
  }
  if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
    s->lead_surrogate = code_unit;
    return GTEXT_DEC_CONTINUE;
  }
  if (code_unit >= 0xDC00 && code_unit <= 0xDFFF) {
    return GTEXT_DEC_ERROR;
  }
  out->cp[0] = code_unit;
  out->n = 1;
  return GTEXT_DEC_EMIT;
}

/* ---- replacement (section 14.1.1) -------------------------------------------------------- */

static void replacement_init(GTEXT_Dec_State * state, GTEXT_Encoding encoding) {
  (void)encoding;
  state->replacement_error_returned = false;
}

static GTEXT_Dec_Result replacement_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  (void)out;
  if (byte < 0) {
    return GTEXT_DEC_FINISHED;
  }
  if (!state->replacement_error_returned) {
    state->replacement_error_returned = true;
    return GTEXT_DEC_ERROR;
  }
  return GTEXT_DEC_FINISHED;
}

/* ---- x-user-defined (section 14.5.1) ------------------------------------------------------ */

static void no_init(GTEXT_Dec_State * state, GTEXT_Encoding encoding) {
  (void)state;
  (void)encoding;
}

static bool always_idle(const GTEXT_Dec_State * state) {
  (void)state;
  return true;
}

static GTEXT_Dec_Result x_user_defined_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  (void)state;
  if (byte < 0) {
    return GTEXT_DEC_FINISHED;
  }
  out->cp[0] = byte <= 0x7F ? (uint32_t)byte : 0xF780u + (uint32_t)byte - 0x80u;
  out->n = 1;
  return GTEXT_DEC_EMIT;
}

/* ---- single-byte encodings (section 9.1) -------------------------------------------------- */

static void single_byte_init(GTEXT_Dec_State * state, GTEXT_Encoding encoding) {
  state->sb_table = gtext_encoding_sb_decode[gtext_encoding_sb_index[encoding]];
}

static GTEXT_Dec_Result single_byte_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out) {
  if (byte < 0) {
    return GTEXT_DEC_FINISHED;
  }
  if (byte <= 0x7F) {
    out->cp[0] = (uint32_t)byte;
    out->n = 1;
    return GTEXT_DEC_EMIT;
  }
  const uint16_t cp = state->sb_table[byte - 0x80];
  if (cp == 0xFFFF) { /* the index has no code point for this byte */
    return GTEXT_DEC_ERROR;
  }
  out->cp[0] = cp;
  out->n = 1;
  return GTEXT_DEC_EMIT;
}

/* ---- the codecs --------------------------------------------------------------------------- */

static const GTEXT_Dec_Codec codec_utf8 = {gtext_utf8_init, gtext_utf8_step, gtext_utf8_idle};
static const GTEXT_Dec_Codec codec_utf16 = {utf16_init, utf16_step, NULL};
static const GTEXT_Dec_Codec codec_replacement = {replacement_init, replacement_step, NULL};
static const GTEXT_Dec_Codec codec_x_user_defined = {no_init, x_user_defined_step, always_idle};
static const GTEXT_Dec_Codec codec_single_byte = {single_byte_init, single_byte_step, always_idle};

/// The decoder of an encoding, or NULL if this release has none (the table is the one place that says which have).
const GTEXT_Dec_Codec * gtext_encoding_dec_codec(GTEXT_Encoding encoding) {
  if ((int)encoding <= (int)GTEXT_ENCODING_NONE || (int)encoding >= (int)GTEXT_ENCODING_COUNT) {
    return NULL;
  }
  switch (encoding) {
  case GTEXT_ENCODING_UTF_8:
    return &codec_utf8;
  case GTEXT_ENCODING_UTF_16BE:
  case GTEXT_ENCODING_UTF_16LE:
    return &codec_utf16;
  case GTEXT_ENCODING_REPLACEMENT:
    return &codec_replacement;
  case GTEXT_ENCODING_X_USER_DEFINED:
    return &codec_x_user_defined;
  default:
    return gtext_encoding_sb_index[encoding] >= 0 ? &codec_single_byte : gtext_encoding_cjk_dec_codec(encoding);
  }
}

/* ---- the decoder ---------------------------------------------------------------------------- */

/// A streaming decoder: the encoding's handler and state, the byte order mark logic, the position, and the output chunk with its limit.
struct GTEXT_Decoder {
  GTEXT_Allocator alloc;      ///< The allocator the decoder was made with (a copy of the option's).
  GTEXT_Decoder_Options opts; ///< `allocator` is not kept (NULL); `alloc` is the copy.
  GTEXT_Encoding requested;   ///< The encoding the caller asked for.
  GTEXT_Encoding active;      ///< `requested`, or what a byte order mark chose.
  const GTEXT_Dec_Codec * codec; ///< The active encoding's decoder.
  GTEXT_Dec_State state;      ///< Its state.
  size_t pos;                 ///< Offset of the next byte to be taken from the stream.
  uint8_t held[3];            ///< The first bytes of the stream, while a byte order mark is still possible.
  uint8_t held_n;             ///< How many of them.
  bool sniff_pending;         ///< The first bytes are still held back.
  bool strip_pending;         ///< The first item written has not been seen yet (BOM removal).
  bool finished;              ///< The handler said "finished" before the end (replacement); the rest is ignored.
  GTEXT_Out out;              ///< The chunk buffer, the limit and the count of the stream.
};

/// What one call is doing, and what it will report.
typedef struct {
  GTEXT_Encoding_Status status; ///< OK until something fails.
  size_t replacements;          ///< U+FFFD written for ill-formed input.
  size_t error_offset;          ///< Fatal mode: the offset of the byte at which the error was found.
  bool bom_found;               ///< A byte order mark was taken.
  size_t taken;                 ///< Bytes of the call's input taken, set when the call fails.
  size_t steps;                 ///< Handler steps taken.
} Call;

static bool unicode_bom_encoding(GTEXT_Encoding e) {
  return e == GTEXT_ENCODING_UTF_8 || e == GTEXT_ENCODING_UTF_16BE || e == GTEXT_ENCODING_UTF_16LE;
}

/// Begin a stream: the state the decoder has when made, after a flush and after a failure. `keep_handler`: keep ISO-2022-JP's mode and nothing else of the
/// handler (see gtext_decoder_decode() and gtext_encoding_iso_2022_jp_dec_resume()).
static void stream_start(GTEXT_Decoder * dec, bool keep_handler) {
  dec->active = dec->requested;
  dec->codec = gtext_encoding_dec_codec(dec->active);
  if (!keep_handler) {
    dec->codec->init(&dec->state, dec->active);
  } else {
    gtext_encoding_iso_2022_jp_dec_resume(&dec->state);
  }
  dec->pos = 0;
  dec->held_n = 0;
  dec->sniff_pending = dec->opts.bom_sniffing;
  dec->strip_pending = !dec->opts.bom_sniffing && dec->opts.bom_removal && unicode_bom_encoding(dec->active);
  dec->finished = false;
  dec->out.n = 0;
  dec->out.total = 0;
  dec->out.status = GTEXT_ENCODING_OK;
}

static bool dec_put_cp(GTEXT_Decoder * dec, uint32_t cp) {
  if (dec->strip_pending) {
    dec->strip_pending = false;
    if (cp == 0xFEFF) {
      return true;
    }
  }
  return gtext_out_put_cp(&dec->out, cp);
}

static bool dec_put_cp2(GTEXT_Decoder * dec, uint32_t cp0, uint32_t cp1) {
  if (dec->strip_pending) {
    dec->strip_pending = false;
    if (cp0 == 0xFEFF) {
      return gtext_out_put_cp(&dec->out, cp1);
    }
  }
  return gtext_out_put_cp2(&dec->out, cp0, cp1);
}

/// Run `len` bytes of `in`, and the end of the stream if `eoq`, through the handler. Everything the standard's
/// "process a queue" does, for the items it is given.
///
/// On a failure `call->taken` is set to `taken_base`, plus the bytes of `in` taken when `count_in` (the held bytes of a
/// byte order mark were taken from the call's input before, and are not counted again).
static void run_items(GTEXT_Decoder * dec, const unsigned char * in, size_t len, bool eoq, size_t taken_base,
    bool count_in, Call * call) {
  if (dec->finished) {
    return; /* The handler is done; whatever else comes is not read. */
  }
  const GTEXT_Dec_Codec * codec = dec->codec;
  uint8_t pb[GTEXT_ENCODING_PUSHBACK]; /* the restore stack: the top is read next */
  size_t pb_n = 0;
  size_t i = 0;
  size_t offset = dec->pos;
  for (;;) {
    int byte;
    if (pb_n > 0) {
      byte = pb[--pb_n];
    } else if (i < len) {
      if (codec->idle && codec->idle(&dec->state)) {
        size_t j = i;
        while (j < len && in[j] < 0x80) {
          j++;
        }
        if (j > i) {
          dec->strip_pending = false;
          size_t accepted = 0;
          if (!gtext_out_put_run(&dec->out, in + i, j - i, &accepted)) {
            call->status = dec->out.status;
            /* the byte at which it failed is the one after those accepted */
            call->taken = taken_base + (count_in ? i + accepted + 1 : 0);
            return;
          }
          dec->pos += j - i;
          i = j;
          if (i == len) {
            continue;
          }
        }
      }
      byte = in[i++];
      offset = dec->pos++;
    } else if (eoq) {
      byte = -1;
      offset = dec->pos;
    } else {
      break;
    }
    GTEXT_Dec_Step step;
    step.n = 0;
    step.restore_n = 0;
    step.cp[0] = step.cp[1] = 0;
    call->steps++;
    const GTEXT_Dec_Result r = codec->step(&dec->state, byte, &step);
    if (r == GTEXT_DEC_CONTINUE) {
      continue;
    }
    if (r == GTEXT_DEC_EMIT) {
      /* Two code points of one step (Big5's sequences) are written together, so that a limit or a refusing sink never splits a character. */
      const bool ok = step.n == 0 ? true /* an EMIT of nothing writes nothing (and reads no code point the handler never set) */
                    : step.n == 2 ? dec_put_cp2(dec, step.cp[0], step.cp[1]) : dec_put_cp(dec, step.cp[0]);
      if (!ok) {
        call->status = dec->out.status;
        call->taken = taken_base + (count_in ? i : 0);
        return;
      }
      continue;
    }
    if (r == GTEXT_DEC_FINISHED) {
      if (byte < 0) {
        break;
      }
      /* finished before the end of the queue: nothing more is read. */
      dec->finished = true;
      i = len;
      pb_n = 0;
      if (eoq) {
        break;
      }
      continue;
    }
    /* GTEXT_DEC_ERROR */
    if (dec->opts.error_mode == GTEXT_DECODER_FATAL) {
      call->status = GTEXT_ENCODING_E_MALFORMED;
      call->error_offset = offset;
      call->taken = taken_base + (count_in ? i : 0);
      return;
    }
    if (!dec_put_cp(dec, 0xFFFD)) {
      call->status = dec->out.status;
      call->taken = taken_base + (count_in ? i : 0);
      return;
    }
    call->replacements++;
    /* A stack that would overflow is a defect of a handler (see GTEXT_ENCODING_PUSHBACK): reported, not a quietly shorter stream. */
    if (pb_n + step.restore_n > sizeof pb) {
      call->status = GTEXT_ENCODING_E_INTERNAL;
      call->taken = taken_base + (count_in ? i : 0);
      return;
    }
    for (size_t k = step.restore_n; k > 0; k--) {
      pb[pb_n++] = step.restore[k - 1];
    }
  }
}

/// The first bytes of a stream, in `held`: is it a byte order mark yet, or could it still become one?
/// Returns 1 for a mark (and sets *enc and *len), 0 for no mark, -1 for undecided.
static int sniff(const GTEXT_Decoder * dec, GTEXT_Encoding * enc, size_t * len) {
  const uint8_t * h = dec->held;
  const size_t n = dec->held_n;
  if (h[0] == 0xEF) {
    if (n >= 2 && h[1] != 0xBB) {
      return 0;
    }
    if (n >= 3) {
      if (h[2] == 0xBF) {
        *enc = GTEXT_ENCODING_UTF_8;
        *len = 3;
        return 1;
      }
      return 0;
    }
    return -1;
  }
  if (h[0] == 0xFE) {
    if (n < 2) {
      return -1;
    }
    if (h[1] == 0xFF) {
      *enc = GTEXT_ENCODING_UTF_16BE;
      *len = 2;
      return 1;
    }
    return 0;
  }
  if (h[0] == 0xFF) {
    if (n < 2) {
      return -1;
    }
    if (h[1] == 0xFE) {
      *enc = GTEXT_ENCODING_UTF_16LE;
      *len = 2;
      return 1;
    }
    return 0;
  }
  return 0;
}

/// Decide the held bytes: a mark is consumed and chooses the encoding; otherwise they are ordinary input.
static void sniff_resolve(GTEXT_Decoder * dec, int verdict, GTEXT_Encoding enc, size_t len, size_t taken,
    Call * call) {
  dec->sniff_pending = false;
  const size_t n = dec->held_n;
  dec->held_n = 0;
  if (verdict == 1) {
    dec->active = enc;
    dec->codec = gtext_encoding_dec_codec(enc);
    dec->codec->init(&dec->state, enc);
    dec->pos += len;
    call->bom_found = true;
    return;
  }
  uint8_t copy[3];
  memcpy(copy, dec->held, n);
  run_items(dec, copy, n, false, taken, false, call);
}

static GTEXT_Encoding_Status decode_impl(GTEXT_Decoder * dec, const unsigned char * in, size_t len, bool flush,
    Call * call) {
  size_t i = 0;
  while (dec->sniff_pending && i < len) {
    dec->held[dec->held_n++] = in[i++];
    GTEXT_Encoding enc = GTEXT_ENCODING_NONE;
    size_t blen = 0;
    const int verdict = sniff(dec, &enc, &blen);
    if (verdict >= 0) {
      sniff_resolve(dec, verdict, enc, blen, i, call);
      if (call->status != GTEXT_ENCODING_OK) {
        return call->status;
      }
    }
  }
  if (!dec->sniff_pending) {
    run_items(dec, len - i > 0 ? in + i : NULL, len - i, false, i, true, call); /* (no arithmetic on a null pointer) */
    if (call->status != GTEXT_ENCODING_OK) {
      return call->status;
    }
  }
  if (flush) {
    if (dec->sniff_pending) {
      sniff_resolve(dec, 0, GTEXT_ENCODING_NONE, 0, len, call);
      if (call->status != GTEXT_ENCODING_OK) {
        return call->status;
      }
    }
    run_items(dec, NULL, 0, true, len, false, call);
  }
  return call->status;
}

/* ---- the public interface ------------------------------------------------------------------- */

GTEXT_Decoder_Options gtext_decoder_options_default(void) {
  GTEXT_Decoder_Options o;
  memset(&o, 0, sizeof(o));
  o.allocator = gtext_allocator_default();
  o.error_mode = GTEXT_DECODER_REPLACEMENT;
  o.max_output_bytes = GTEXT_ENCODING_DEFAULT_MAX_OUTPUT;
  return o;
}

static void fill_result(GTEXT_Decoder_Result * result, GTEXT_Encoding_Status status, GTEXT_Encoding encoding,
    const Call * call, size_t output_bytes) {
  if (!result) {
    return;
  }
  memset(result, 0, sizeof(*result));
  result->status = status;
  result->message = gtext_encoding_status_string(status);
  result->encoding = encoding;
  if (call) {
    result->input_bytes = call->taken;
    result->replacements = call->replacements;
    result->error_offset = call->error_offset;
    result->bom_found = call->bom_found;
    result->steps = call->steps;
  }
  result->output_bytes = output_bytes;
}

GTEXT_Encoding_Status gtext_decoder_new(GTEXT_Encoding encoding, const GTEXT_Decoder_Options * opts,
    GTEXT_Decoder ** out) {
  if (!out || (int)encoding <= (int)GTEXT_ENCODING_NONE || (int)encoding >= (int)GTEXT_ENCODING_COUNT) {
    return GTEXT_ENCODING_E_INVALID;
  }
  GTEXT_Decoder_Options o;
  memset(&o, 0, sizeof(o));
  if (opts) {
    o = *opts;
  }
  if ((int)o.error_mode < (int)GTEXT_DECODER_REPLACEMENT || (int)o.error_mode > (int)GTEXT_DECODER_FATAL) {
    return GTEXT_ENCODING_E_OPTIONS;
  }
  if (!gtext_encoding_dec_codec(encoding)) {
    return GTEXT_ENCODING_E_UNSUPPORTED;
  }
  GTEXT_Allocator alloc;
  gtext_encoding_allocator_copy(&alloc, o.allocator);
  GTEXT_Decoder * dec = gtext_allocator_malloc(&alloc, sizeof(*dec));
  if (!dec) {
    return GTEXT_ENCODING_E_OOM;
  }
  memset(dec, 0, sizeof(*dec));
  dec->alloc = alloc;
  dec->opts = o;
  dec->opts.allocator = NULL;
  if (dec->opts.max_output_bytes == 0) {
    dec->opts.max_output_bytes = GTEXT_ENCODING_DEFAULT_MAX_OUTPUT;
  }
  dec->requested = encoding;
  dec->out.limit = dec->opts.max_output_bytes;
  stream_start(dec, false);
  *out = dec;
  return GTEXT_ENCODING_OK;
}

void gtext_decoder_free(GTEXT_Decoder * decoder) {
  if (!decoder) {
    return;
  }
  GTEXT_Allocator alloc = decoder->alloc;
  gtext_allocator_free(&alloc, decoder);
}

void gtext_decoder_reset(GTEXT_Decoder * decoder) {
  if (decoder) {
    stream_start(decoder, false);
  }
}

GTEXT_Encoding gtext_decoder_encoding(const GTEXT_Decoder * decoder) {
  return decoder ? decoder->active : GTEXT_ENCODING_NONE;
}

GTEXT_Encoding_Status gtext_decoder_decode(GTEXT_Decoder * dec, const void * in, size_t len, bool flush,
    const GTEXT_Encoding_Sink * sink, GTEXT_Decoder_Result * result) {
  if (!dec || !sink || !sink->write || (!in && len > 0)) {
    fill_result(result, GTEXT_ENCODING_E_INVALID, dec ? dec->active : GTEXT_ENCODING_NONE, NULL, 0);
    return GTEXT_ENCODING_E_INVALID;
  }
  Call call;
  memset(&call, 0, sizeof(call));
  call.status = GTEXT_ENCODING_OK;
  dec->out.sink = sink;
  dec->out.delivered = 0;
  dec->out.status = GTEXT_ENCODING_OK;
  GTEXT_Encoding_Status status = decode_impl(dec, (const unsigned char *)in, len, flush, &call);
  if (status == GTEXT_ENCODING_OK) {
    call.taken = len;
  }
  /* The sink gets what was produced, also before a failure; but not after it refused. */
  if (status != GTEXT_ENCODING_E_WRITE) {
    if (!gtext_out_flush(&dec->out) && status == GTEXT_ENCODING_OK) {
      status = GTEXT_ENCODING_E_WRITE;
    }
  }
  const GTEXT_Encoding used = dec->active;
  const size_t delivered = dec->out.delivered;
  if (status == GTEXT_ENCODING_OK && !flush) {
    fill_result(result, status, used, &call, delivered);
    return status;
  }
  fill_result(result, status, used, &call, delivered);
  /* The stream is over (a flush or a failure) and the next call begins a new one - with one exception the standard's own tests insist on: "ISO-2022-JP ... is
     the only decoder which does not clear internal state before throwing in stream mode", so after a fatal error in a call that did not flush, its handler
     goes on in the mode it was in (the Roman or katakana state an escape sequence selected). The other encodings start over, as before: their handlers clear their
     own state when they return an error (all but `replacement`'s, whose first error would then be the last); the driver's own state (positions, byte order mark
     handling, the count of output) always starts over. */
  stream_start(dec, status == GTEXT_ENCODING_E_MALFORMED && !flush && dec->active == GTEXT_ENCODING_ISO_2022_JP);
  return status;
}

GTEXT_Encoding_Status gtext_encoding_decode_buffer(GTEXT_Encoding encoding, const GTEXT_Decoder_Options * opts,
    const void * in, size_t len, char ** out, size_t * out_len, GTEXT_Decoder_Result * result) {
  if (!out || (!in && len > 0)) {
    fill_result(result, GTEXT_ENCODING_E_INVALID, encoding, NULL, 0);
    return GTEXT_ENCODING_E_INVALID;
  }
  GTEXT_Decoder * dec = NULL;
  GTEXT_Encoding_Status status = gtext_decoder_new(encoding, opts, &dec);
  if (status != GTEXT_ENCODING_OK) {
    fill_result(result, status, encoding, NULL, 0);
    return status;
  }
  GTEXT_Encoding_Buffer buf;
  memset(&buf, 0, sizeof(buf));
  buf.alloc = dec->alloc;
  const GTEXT_Encoding_Sink sink = {gtext_encoding_buffer_write, &buf};
  status = gtext_decoder_decode(dec, in, len, true, &sink, result);
  if (status == GTEXT_ENCODING_E_WRITE && buf.oom) {
    status = GTEXT_ENCODING_E_OOM;
    if (result) {
      result->status = status;
      result->message = gtext_encoding_status_string(status);
    }
  }
  if (status == GTEXT_ENCODING_OK && !buf.base) { /* an empty output still has its NUL */
    gtext_encoding_buffer_write(&buf, "", 0);
    if (!buf.base) {
      status = GTEXT_ENCODING_E_OOM;
      if (result) {
        result->status = status;
        result->message = gtext_encoding_status_string(status);
      }
    }
  }
  gtext_decoder_free(dec);
  if (status != GTEXT_ENCODING_OK) {
    gtext_allocator_free(&buf.alloc, buf.base);
    return status;
  }
  *out = (char *)buf.base + GTEXT_ENCODING_BUF_HEADER;
  if (out_len) {
    *out_len = buf.length;
  }
  return GTEXT_ENCODING_OK;
}
