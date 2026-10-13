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
 * The encoders: the standard's handlers for UTF-8, `x-user-defined` and the
 * single-byte encodings (sections 8.1.2, 9.2 and 14.5.2), and the driver that
 * reads UTF-8 from the caller, runs a handler over the code points and applies
 * the error mode ("process an item", section 4.1, for an encoder).
 */

#include <ghoti.io/text/encoding.h>

#include "encoding_internal.h"

#include <stdint.h>
#include <string.h>

/* ---- the handlers ------------------------------------------------------------------------- */

static void enc_no_init(GTEXT_Enc_State * state, GTEXT_Encoding encoding) {
  (void)state;
  (void)encoding;
}

/// UTF-8's encoder (section 8.1.2).
static GTEXT_Enc_Result utf8_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  (void)state;
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  if (cp <= 0x7F) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  unsigned count;
  unsigned offset;
  if (cp <= 0x7FF) {
    count = 1;
    offset = 0xC0;
  } else if (cp <= 0xFFFF) {
    count = 2;
    offset = 0xE0;
  } else {
    count = 3;
    offset = 0xF0;
  }
  size_t n = 0;
  out->bytes[n++] = (uint8_t)(((uint32_t)cp >> (6 * count)) + offset);
  while (count > 0) {
    const uint32_t temp = (uint32_t)cp >> (6 * (count - 1));
    out->bytes[n++] = (uint8_t)(0x80 | (temp & 0x3F));
    count--;
  }
  out->n = (uint8_t)n;
  return GTEXT_ENC_OK;
}

/// x-user-defined's encoder (section 14.5.2).
static GTEXT_Enc_Result x_user_defined_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  (void)state;
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  if (cp <= 0x7F) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  if (cp >= 0xF780 && cp <= 0xF7FF) {
    out->bytes[0] = (uint8_t)(cp - 0xF780 + 0x80);
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  return GTEXT_ENC_ERROR;
}

static void single_byte_enc_init(GTEXT_Enc_State * state, GTEXT_Encoding encoding) {
  const int t = gtext_encoding_sb_index[encoding];
  state->sb.cps = gtext_encoding_sb_encode_cp[t];
  state->sb.bytes = gtext_encoding_sb_encode_byte[t];
  state->sb.count = gtext_encoding_sb_encode_count[t];
}

/// The single-byte encoders' handler (section 9.2): "the index pointer for codePoint in index single-byte" is a
/// binary search over the generated reverse table, which holds the first pointer of each code point.
static GTEXT_Enc_Result single_byte_enc_step(GTEXT_Enc_State * state, int32_t cp, GTEXT_Enc_Step * out) {
  if (cp < 0) {
    return GTEXT_ENC_FINISHED;
  }
  if (cp <= 0x7F) {
    out->bytes[0] = (uint8_t)cp;
    out->n = 1;
    return GTEXT_ENC_OK;
  }
  if (cp > 0xFFFF) {
    return GTEXT_ENC_ERROR;
  }
  size_t lo = 0;
  size_t hi = state->sb.count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const uint16_t c = state->sb.cps[mid];
    if (c == (uint16_t)cp) {
      out->bytes[0] = state->sb.bytes[mid];
      out->n = 1;
      return GTEXT_ENC_OK;
    }
    if (c < (uint16_t)cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return GTEXT_ENC_ERROR;
}

static const GTEXT_Enc_Codec enc_utf8 = {enc_no_init, utf8_enc_step, true};
static const GTEXT_Enc_Codec enc_x_user_defined = {enc_no_init, x_user_defined_enc_step, true};
static const GTEXT_Enc_Codec enc_single_byte = {single_byte_enc_init, single_byte_enc_step, true};

/// The encoder of an encoding, or NULL if it has none or this release has none.
const GTEXT_Enc_Codec * gtext_encoding_enc_codec(GTEXT_Encoding encoding) {
  if ((int)encoding <= (int)GTEXT_ENCODING_NONE || (int)encoding >= (int)GTEXT_ENCODING_COUNT) {
    return NULL;
  }
  switch (encoding) {
  case GTEXT_ENCODING_UTF_8:
    return &enc_utf8;
  case GTEXT_ENCODING_X_USER_DEFINED:
    return &enc_x_user_defined;
  default:
    return gtext_encoding_sb_index[encoding] >= 0 ? &enc_single_byte : gtext_encoding_cjk_enc_codec(encoding);
  }
}

/* ---- the encoder ----------------------------------------------------------------------------- */

/// A streaming encoder: the encoding's handler and state, the UTF-8 reader of its input, the position, and the output chunk with its limit.
struct GTEXT_Encoder {
  GTEXT_Allocator alloc;      ///< The allocator the encoder was made with (a copy of the option's).
  GTEXT_Encoder_Options opts; ///< `allocator` is not kept (NULL); `alloc` is the copy.
  GTEXT_Encoding encoding;    ///< The encoding written.
  const GTEXT_Enc_Codec * codec; ///< Its encoder.
  GTEXT_Enc_State state;      ///< Its state.
  GTEXT_Dec_State input;      ///< The UTF-8 decoder that reads the caller's bytes.
  size_t pos;                 ///< Offset of the next byte to be taken from the stream.
  size_t seq_start;           ///< Offset of the first byte of the character being read.
  GTEXT_Out out;              ///< The chunk buffer, the limit and the count of the stream.
};

/// What one call is doing, and what it will report.
typedef struct {
  GTEXT_Encoding_Status status; ///< OK until something fails.
  size_t replacements;          ///< Code points written as a reference or `?`, and malformed input written as U+FFFD.
  size_t error_offset;          ///< Fatal mode: the offset of the character (or of the byte at which the malformation was found).
  uint32_t error_code_point;    ///< Fatal mode: the code point that could not be written.
  size_t taken;                 ///< Bytes of the call's input taken, set when the call fails.
  size_t steps;                 ///< Handler steps taken.
} Call;

static void stream_start(GTEXT_Encoder * enc) {
  enc->codec->init(&enc->state, enc->encoding);
  gtext_utf8_init(&enc->input, GTEXT_ENCODING_UTF_8);
  enc->pos = 0;
  enc->seq_start = 0;
  enc->out.n = 0;
  enc->out.total = 0;
  enc->out.status = GTEXT_ENCODING_OK;
}

/// Write "&#" and the decimal number and ";".
static bool put_reference(GTEXT_Out * out, uint32_t cp) {
  unsigned char b[16];
  size_t n = 0;
  b[n++] = '&';
  b[n++] = '#';
  unsigned char digits[10];
  size_t d = 0;
  do {
    digits[d++] = (unsigned char)('0' + cp % 10);
    cp /= 10;
  } while (cp > 0);
  while (d > 0) {
    b[n++] = digits[--d];
  }
  b[n++] = ';';
  return gtext_out_put(out, b, n);
}

/// Encode one code point: the handler, then the error mode if the handler refuses it. `counted`: the code point
/// stands for malformed input that has been counted already.
static bool encode_cp(GTEXT_Encoder * enc, uint32_t cp, bool counted, Call * call) {
  for (int round = 0; round < 3; round++) {
    GTEXT_Enc_Step step;
    step.n = 0;
    step.restore = false;
    step.restore_cp = cp; /* a handler that restores without naming a code point gives back the one it was given, never U+0000 */
    step.error_cp = cp;
    call->steps++;
    const GTEXT_Enc_Result r = enc->codec->step(&enc->state, (int32_t)cp, &step);
    if (r == GTEXT_ENC_OK) {
      if (step.n > 0 && !gtext_out_put(&enc->out, step.bytes, step.n)) {
        call->status = enc->out.status;
        return false;
      }
      if (!step.restore) {
        return true;
      }
      cp = step.restore_cp; /* "restore codePoint to ioQueue": what the handler gave back, which it may have changed */
      continue;
    }
    if (r == GTEXT_ENC_FINISHED) {
      return true;
    }
    /* ERROR: the code point cannot be represented. The standard's "error with codePoint" is `step.error_cp` (ISO-2022-JP: U+FFFD for the three controls it
       will not write, "rather than codePoint to prevent attacks"), and it is what the HTML mode writes as a reference. The fatal status names the code point
       that was handled, so that a caller can tell which character it was (a control, for ISO-2022-JP, not U+FFFD). */
    switch (enc->opts.error_mode) {
    case GTEXT_ENCODER_HTML:
      if (!put_reference(&enc->out, step.error_cp)) {
        call->status = enc->out.status;
        return false;
      }
      break;
    case GTEXT_ENCODER_REPLACEMENT:
      if (!gtext_out_put(&enc->out, "?", 1)) {
        call->status = enc->out.status;
        return false;
      }
      break;
    default:
      call->status = GTEXT_ENCODING_E_UNENCODABLE;
      call->error_code_point = cp;
      call->error_offset = enc->seq_start;
      return false;
    }
    if (!counted) {
      call->replacements++;
    }
    return true;
  }
  /* A handler that keeps asking for the same code point again is a defect; the call stops, with a status that says so, rather than loops. */
  call->status = GTEXT_ENCODING_E_INTERNAL;
  return false;
}

static void run_input(GTEXT_Encoder * enc, const unsigned char * in, size_t len, bool eoq, Call * call) {
  uint8_t pb[GTEXT_ENCODING_PUSHBACK];
  size_t pb_n = 0;
  size_t i = 0;
  size_t offset = enc->pos;
  for (;;) {
    int byte;
    if (pb_n > 0) {
      byte = pb[--pb_n];
      if (enc->input.utf8.bytes_needed == 0) {
        enc->seq_start = offset; /* a restored byte is the one just taken */
      }
    } else if (i < len) {
      if (enc->codec->ascii_passthrough && enc->input.utf8.bytes_needed == 0) {
        size_t j = i;
        while (j < len && in[j] < 0x80) {
          j++;
        }
        if (j > i) {
          size_t accepted = 0;
          if (!gtext_out_put_run(&enc->out, in + i, j - i, &accepted)) {
            call->status = enc->out.status;
            call->taken = i + accepted + 1; /* the byte at which it failed is the one after those accepted */
            return;
          }
          enc->pos += j - i;
          i = j;
          if (i == len) {
            continue;
          }
        }
      }
      byte = in[i++];
      offset = enc->pos++;
      if (enc->input.utf8.bytes_needed == 0) {
        enc->seq_start = offset;
      }
    } else if (eoq) {
      byte = -1;
      offset = enc->pos;
    } else {
      break;
    }
    GTEXT_Dec_Step step;
    step.n = 0;
    step.restore_n = 0;
    step.cp[0] = step.cp[1] = 0;
    call->steps++;
    const GTEXT_Dec_Result r = gtext_utf8_step(&enc->input, byte, &step);
    if (r == GTEXT_DEC_CONTINUE) {
      continue;
    }
    if (r == GTEXT_DEC_EMIT) {
      if (step.n > 0 && !encode_cp(enc, step.cp[0], false, call)) {
        call->taken = i;
        return;
      }
      continue;
    }
    if (r == GTEXT_DEC_FINISHED) {
      break;
    }
    /* The input is not well-formed UTF-8 here: it is U+FFFD, or in fatal mode a failure. */
    if (enc->opts.error_mode == GTEXT_ENCODER_FATAL) {
      call->status = GTEXT_ENCODING_E_MALFORMED;
      call->error_offset = offset;
      call->taken = i;
      return;
    }
    call->replacements++;
    if (!encode_cp(enc, 0xFFFD, true, call)) {
      call->taken = i;
      return;
    }
    if (pb_n + step.restore_n > sizeof pb) {
      call->status = GTEXT_ENCODING_E_INTERNAL; /* a defect of a handler: see GTEXT_ENCODING_PUSHBACK */
      call->taken = i;
      return;
    }
    for (size_t k = step.restore_n; k > 0; k--) {
      pb[pb_n++] = step.restore[k - 1];
    }
  }
  if (eoq) {
    /* The end of the code points: a stateful encoder says what closes its output. */
    GTEXT_Enc_Step step;
    step.n = 0;
    step.restore = false;
    if (enc->codec->step(&enc->state, -1, &step) == GTEXT_ENC_OK && step.n > 0
        && !gtext_out_put(&enc->out, step.bytes, step.n)) {
      call->status = enc->out.status;
      call->taken = len;
    }
  }
}

/* ---- the public interface --------------------------------------------------------------------- */

GTEXT_Encoder_Options gtext_encoder_options_default(void) {
  GTEXT_Encoder_Options o;
  memset(&o, 0, sizeof(o));
  o.allocator = gtext_allocator_default();
  o.error_mode = GTEXT_ENCODER_FATAL;
  o.max_output_bytes = GTEXT_ENCODING_DEFAULT_MAX_OUTPUT;
  return o;
}

static void fill_result(GTEXT_Encoder_Result * result, GTEXT_Encoding_Status status, const Call * call,
    size_t output_bytes) {
  if (!result) {
    return;
  }
  memset(result, 0, sizeof(*result));
  result->status = status;
  result->message = gtext_encoding_status_string(status);
  if (call) {
    result->input_bytes = call->taken;
    result->replacements = call->replacements;
    result->error_offset = call->error_offset;
    result->error_code_point = call->error_code_point;
    result->steps = call->steps;
  }
  result->output_bytes = output_bytes;
}

GTEXT_Encoding_Status gtext_encoder_new(GTEXT_Encoding encoding, const GTEXT_Encoder_Options * opts,
    GTEXT_Encoder ** out) {
  if (!out || (int)encoding <= (int)GTEXT_ENCODING_NONE || (int)encoding >= (int)GTEXT_ENCODING_COUNT) {
    return GTEXT_ENCODING_E_INVALID;
  }
  GTEXT_Encoder_Options o;
  memset(&o, 0, sizeof(o));
  if (opts) {
    o = *opts;
  }
  if ((int)o.error_mode < (int)GTEXT_ENCODER_FATAL || (int)o.error_mode > (int)GTEXT_ENCODER_REPLACEMENT) {
    return GTEXT_ENCODING_E_OPTIONS;
  }
  if (encoding == GTEXT_ENCODING_REPLACEMENT || encoding == GTEXT_ENCODING_UTF_16BE
      || encoding == GTEXT_ENCODING_UTF_16LE) {
    return GTEXT_ENCODING_E_NO_ENCODER;
  }
  if (!gtext_encoding_enc_codec(encoding)) {
    return GTEXT_ENCODING_E_UNSUPPORTED;
  }
  GTEXT_Allocator alloc;
  gtext_encoding_allocator_copy(&alloc, o.allocator);
  GTEXT_Encoder * enc = gtext_allocator_malloc(&alloc, sizeof(*enc));
  if (!enc) {
    return GTEXT_ENCODING_E_OOM;
  }
  memset(enc, 0, sizeof(*enc));
  enc->alloc = alloc;
  enc->opts = o;
  enc->opts.allocator = NULL;
  if (enc->opts.max_output_bytes == 0) {
    enc->opts.max_output_bytes = GTEXT_ENCODING_DEFAULT_MAX_OUTPUT;
  }
  enc->encoding = encoding;
  enc->codec = gtext_encoding_enc_codec(encoding);
  enc->out.limit = enc->opts.max_output_bytes;
  stream_start(enc);
  *out = enc;
  return GTEXT_ENCODING_OK;
}

void gtext_encoder_free(GTEXT_Encoder * encoder) {
  if (!encoder) {
    return;
  }
  GTEXT_Allocator alloc = encoder->alloc;
  gtext_allocator_free(&alloc, encoder);
}

void gtext_encoder_reset(GTEXT_Encoder * encoder) {
  if (encoder) {
    stream_start(encoder);
  }
}

GTEXT_Encoding gtext_encoder_encoding(const GTEXT_Encoder * encoder) {
  return encoder ? encoder->encoding : GTEXT_ENCODING_NONE;
}

GTEXT_Encoding_Status gtext_encoder_encode(GTEXT_Encoder * enc, const void * in, size_t len, bool flush,
    const GTEXT_Encoding_Sink * sink, GTEXT_Encoder_Result * result) {
  if (!enc || !sink || !sink->write || (!in && len > 0)) {
    fill_result(result, GTEXT_ENCODING_E_INVALID, NULL, 0);
    return GTEXT_ENCODING_E_INVALID;
  }
  Call call;
  memset(&call, 0, sizeof(call));
  call.status = GTEXT_ENCODING_OK;
  enc->out.sink = sink;
  enc->out.delivered = 0;
  enc->out.status = GTEXT_ENCODING_OK;
  run_input(enc, (const unsigned char *)in, len, flush, &call);
  GTEXT_Encoding_Status status = call.status;
  if (status == GTEXT_ENCODING_OK) {
    call.taken = len;
  }
  if (status != GTEXT_ENCODING_E_WRITE) {
    if (!gtext_out_flush(&enc->out) && status == GTEXT_ENCODING_OK) {
      status = GTEXT_ENCODING_E_WRITE;
    }
  }
  fill_result(result, status, &call, enc->out.delivered);
  if (status != GTEXT_ENCODING_OK || flush) {
    stream_start(enc);
  }
  return status;
}

GTEXT_Encoding_Status gtext_encoding_encode_buffer(GTEXT_Encoding encoding, const GTEXT_Encoder_Options * opts,
    const void * in, size_t len, char ** out, size_t * out_len, GTEXT_Encoder_Result * result) {
  if (!out || (!in && len > 0)) {
    fill_result(result, GTEXT_ENCODING_E_INVALID, NULL, 0);
    return GTEXT_ENCODING_E_INVALID;
  }
  GTEXT_Encoder * enc = NULL;
  GTEXT_Encoding_Status status = gtext_encoder_new(encoding, opts, &enc);
  if (status != GTEXT_ENCODING_OK) {
    fill_result(result, status, NULL, 0);
    return status;
  }
  GTEXT_Encoding_Buffer buf;
  memset(&buf, 0, sizeof(buf));
  buf.alloc = enc->alloc;
  const GTEXT_Encoding_Sink sink = {gtext_encoding_buffer_write, &buf};
  status = gtext_encoder_encode(enc, in, len, true, &sink, result);
  if (status == GTEXT_ENCODING_E_WRITE && buf.oom) {
    status = GTEXT_ENCODING_E_OOM;
    if (result) {
      result->status = status;
      result->message = gtext_encoding_status_string(status);
    }
  }
  if (status == GTEXT_ENCODING_OK && !buf.base) {
    gtext_encoding_buffer_write(&buf, "", 0);
    if (!buf.base) {
      status = GTEXT_ENCODING_E_OOM;
      if (result) {
        result->status = status;
        result->message = gtext_encoding_status_string(status);
      }
    }
  }
  gtext_encoder_free(enc);
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
