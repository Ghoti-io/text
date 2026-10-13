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
 * What the files of the Encoding module share: the generated tables' shape, the
 * step interface every decoder and encoder implements, and the output buffer.
 *
 * ## How an encoding plugs in
 *
 * The public interface (include/ghoti.io/text/encoding/) names no encoding but
 * through the GTEXT_Encoding enum and speaks of no algorithm. Behind it, an encoding is a row
 * of two small tables - GTEXT_Dec_Codec and GTEXT_Enc_Codec - and a function or two,
 * following the standard's own formulation: a *handler* is given one item and returns
 * what the standard's handlers return.
 *
 * - A decoder's handler takes a byte (or -1, the standard's end-of-queue) and
 *   returns CONTINUE, EMIT (one or two code points), ERROR, or FINISHED. An ERROR
 *   (and only an ERROR) may ask for up to four bytes to be *restored*, which the driver
 *   puts back in front of the input, in a small pushback stack, so the standard's
 *   "restore byte to ioQueue" and "restore bytes to ioQueue" need no look-behind of
 *   their own. The driver owns the rest: the output buffer, the limits, the two error
 *   modes, the byte order mark handling, chunk boundaries and the ASCII fast path.
 * - An encoder's handler takes a code point (or -1) and returns OK with up to eight
 *   bytes, ERROR (the code point is unencodable), or FINISHED, and may ask for the
 *   same code point to be handled again (ISO-2022-JP's "prepend code point"). The driver owns the UTF-8 reading,
 *   the three error modes and the buffer.
 *
 * The multi-byte stage adds rows and handlers, and their index tables to the
 * generated file. It does not touch the public headers, the drivers or the callers.
 */

#ifndef GHOTI_IO_GTEXT_SRC_ENCODING_ENCODING_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_ENCODING_ENCODING_INTERNAL_H

#include <ghoti.io/text/encoding.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the generated tables (tables/encoding_tables.c) ---------------------------------- */

/// The number of single-byte indexes (the generator checks that this is what it wrote).
#define GTEXT_ENCODING_SB_TABLES 27

/// One label of the sorted table: where it is in the pool, its length, the encoding it names.
typedef struct {
  uint16_t offset;
  uint8_t length;
  uint8_t encoding; ///< A GTEXT_Encoding.
} GTEXT_Encoding_Label_Entry;

/// The standard's name of each encoding, indexed by GTEXT_Encoding (a fixed slot each, so the table has no relocations).
extern const char gtext_encoding_names[GTEXT_ENCODING_COUNT][16];
/// How many labels the table holds.
extern const size_t gtext_encoding_label_total;
/// The length of the longest label, which bounds what a lookup has to look at.
extern const size_t gtext_encoding_label_longest;
/// The labels, lowercase, run together without terminators.
extern const char gtext_encoding_label_pool[];
/// The labels sorted by bytes: where each is in the pool, its length and its encoding.
extern const GTEXT_Encoding_Label_Entry gtext_encoding_label_table[];
/// The single-byte table of an encoding, or -1.
extern const int8_t gtext_encoding_sb_index[GTEXT_ENCODING_COUNT];
/// byte - 0x80 to code point; 0xFFFF for none.
extern const uint16_t gtext_encoding_sb_decode[GTEXT_ENCODING_SB_TABLES][128];
/// How many code points each single-byte index has (the length of its encode arrays).
extern const uint8_t gtext_encoding_sb_encode_count[GTEXT_ENCODING_SB_TABLES];
/// The code points of an index, ascending, and the byte of each.
extern const uint16_t gtext_encoding_sb_encode_cp[GTEXT_ENCODING_SB_TABLES][128];
/// The byte of each code point of gtext_encoding_sb_encode_cp.
extern const uint8_t gtext_encoding_sb_encode_byte[GTEXT_ENCODING_SB_TABLES][128];

/* ---- decoders ------------------------------------------------------------------------- */

/// What a decoder handler returns (the standard's "finished", "one or more items", "error", "continue").
typedef enum {
  GTEXT_DEC_CONTINUE = 0,
  GTEXT_DEC_EMIT,
  GTEXT_DEC_ERROR,
  GTEXT_DEC_FINISHED
} GTEXT_Dec_Result;

/// What a handler produced besides its return value.
typedef struct {
  uint32_t cp[2];         ///< EMIT: the code points (Big5 and gb18030 can return two).
  uint8_t n;              ///< EMIT: how many.
  uint8_t restore_n;      ///< ERROR: bytes to put back in front of the input.
  uint8_t restore[4];     ///< ... first to be read first (GTEXT_ENCODING_RESTORE_MAX of them).
} GTEXT_Dec_Step;

/// UTF-8's decoder state (section 8.1.1).
typedef struct {
  uint32_t code_point;
  uint8_t bytes_seen;
  uint8_t bytes_needed;
  uint8_t lower;
  uint8_t upper;
} GTEXT_Utf8_State;

/// The shared UTF-16 decoder's state (section 14.2.1).
typedef struct {
  int32_t lead_byte;      ///< -1 for null.
  uint32_t lead_surrogate; ///< 0 for null.
  bool big_endian;
} GTEXT_Utf16_State;

/// A decoder's state. The union grows with the encodings; the driver treats it as opaque.
typedef union {
  GTEXT_Utf8_State utf8;
  GTEXT_Utf16_State utf16;
  bool replacement_error_returned;
  const uint16_t * sb_table;
} GTEXT_Dec_State;

typedef GTEXT_Dec_Result (*GTEXT_Dec_Handler)(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out);

/// An encoding's decoder: how to start one, how to step it, and what the driver may assume.
typedef struct {
  void (*init)(GTEXT_Dec_State * state, GTEXT_Encoding encoding);
  GTEXT_Dec_Handler step;
  /// While `idle` is true, a byte below 0x80 is the code point of that value and changes nothing: the
  /// driver then copies a run of them without calling the handler. NULL: never.
  bool (*idle)(const GTEXT_Dec_State * state);
} GTEXT_Dec_Codec;

/// The decoder of an encoding, or NULL if this release has none.
const GTEXT_Dec_Codec * gtext_encoding_dec_codec(GTEXT_Encoding encoding);

/* ---- encoders ------------------------------------------------------------------------- */

typedef enum {
  GTEXT_ENC_OK = 0,
  GTEXT_ENC_ERROR,     ///< The code point is unencodable.
  GTEXT_ENC_FINISHED
} GTEXT_Enc_Result;

typedef struct {
  uint8_t bytes[8];
  uint8_t n;
  bool restore;        ///< Handle the same code point again after writing `bytes`.
} GTEXT_Enc_Step;

typedef union {
  struct {                    ///< A single-byte encoder: its index, sorted by code point.
    const uint16_t * cps;
    const uint8_t * bytes;
    size_t count;
  } sb;
  uint32_t reserved[4];       ///< Room for the state of the multi-byte stage's encoders (ISO-2022-JP keeps its mode, two words at most); the union is the size of its largest member.
} GTEXT_Enc_State;

typedef GTEXT_Enc_Result (*GTEXT_Enc_Handler)(GTEXT_Enc_State * state, int32_t code_point, GTEXT_Enc_Step * out);

typedef struct {
  void (*init)(GTEXT_Enc_State * state, GTEXT_Encoding encoding);
  GTEXT_Enc_Handler step;
  /// An ASCII byte is its own encoding at any time (no state): the driver then copies runs of ASCII.
  bool ascii_passthrough;
} GTEXT_Enc_Codec;

/// The encoder of an encoding, or NULL.
const GTEXT_Enc_Codec * gtext_encoding_enc_codec(GTEXT_Encoding encoding);

/* ---- shared by the drivers -------------------------------------------------------------- */

/// The UTF-8 decoder's handler and initializer (used by the UTF-8 decoder and by every encoder, for its input).
/// UTF-8's decoder handler (section 8.1.1), which every encoder also uses to read its UTF-8 input.
GTEXT_Dec_Result gtext_utf8_step(GTEXT_Dec_State * state, int byte, GTEXT_Dec_Step * out);
/// Start UTF-8's decoder.
void gtext_utf8_init(GTEXT_Dec_State * state, GTEXT_Encoding encoding);
/// Is UTF-8's decoder between characters, so that an ASCII byte is the next code point?
bool gtext_utf8_idle(const GTEXT_Dec_State * state);

/// Most bytes the restore stack of a driver holds. A handler restores at most GTEXT_ENCODING_RESTORE_MAX (the arrays of GTEXT_Dec_Step), and a
/// restored byte re-read puts nothing back in the encodings of this release (UTF-8 restores one byte, UTF-16 two, which are then a lead byte and a
/// unit that cannot fail again); a handler of the multi-byte stage that needs more makes the call GTEXT_ENCODING_E_INTERNAL, not a truncated stream.
#define GTEXT_ENCODING_PUSHBACK 16
/// Most bytes one handler step restores.
#define GTEXT_ENCODING_RESTORE_MAX 4
_Static_assert(GTEXT_ENCODING_RESTORE_MAX <= GTEXT_ENCODING_PUSHBACK, "a step must fit the stack");

/// The longest label the lookup can hold in its lowercase key (the generator refuses a longer label).
#define GTEXT_ENCODING_LABEL_KEY 64

/// Default for max_output_bytes: 256 MiB.
#define GTEXT_ENCODING_DEFAULT_MAX_OUTPUT ((size_t)256 * 1024 * 1024)

/// Bytes the chunk buffer holds; the sink is called with up to this many at a time.
#define GTEXT_ENCODING_CHUNK 4096

/**
 * The output side: a chunk buffer in front of the sink, the byte count and the limit. Whole code points (or whole
 * encoded characters) go in with out_put(), which is how a chunk is never cut inside one and how a limit is never
 * crossed halfway through one.
 */
typedef struct {
  unsigned char buf[GTEXT_ENCODING_CHUNK];
  size_t n;                     ///< Bytes in `buf`.
  size_t total;                 ///< Bytes put since the start of the stream.
  size_t limit;
  size_t delivered;             ///< Bytes given to the sink during this call.
  const GTEXT_Encoding_Sink * sink;
  GTEXT_Encoding_Status status; ///< OK until a put or flush fails.
} GTEXT_Out;

/// Give the buffered bytes to the sink. false (with `status` set) if it refused.
bool gtext_out_flush(GTEXT_Out * out);

/// Add `n` (at most 16) bytes; false (with `status` set) if the limit would be crossed or the sink refused.
static inline bool gtext_out_put(GTEXT_Out * out, const void * bytes, size_t n) {
  if (n > out->limit - out->total) {
    out->status = GTEXT_ENCODING_E_LIMIT_OUTPUT;
    return false;
  }
  if (n > GTEXT_ENCODING_CHUNK - out->n && !gtext_out_flush(out)) {
    return false;
  }
  memcpy(out->buf + out->n, bytes, n);
  out->n += n;
  out->total += n;
  return true;
}

/// Add a run of any length (the ASCII fast paths): the same, in pieces; the part that fits the limit is delivered before the failure.
/// `accepted` receives how many bytes were taken, so that the byte at which a limit or a sink failed is exact: it is the one after them.
bool gtext_out_put_run(GTEXT_Out * out, const unsigned char * bytes, size_t n, size_t * accepted);

/// Add the UTF-8 of a scalar value.
static inline bool gtext_out_put_cp(GTEXT_Out * out, uint32_t cp) {
  unsigned char b[4];
  size_t n;
  if (cp < 0x80) {
    b[0] = (unsigned char)cp;
    n = 1;
  } else if (cp < 0x800) {
    b[0] = (unsigned char)(0xC0 | (cp >> 6));
    b[1] = (unsigned char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    b[0] = (unsigned char)(0xE0 | (cp >> 12));
    b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (unsigned char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    b[0] = (unsigned char)(0xF0 | (cp >> 18));
    b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (unsigned char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  return gtext_out_put(out, b, n);
}

/// A buffer in front of which the allocator it was made with is kept (see gtext_encoding_buffer_free()).
#define GTEXT_ENCODING_BUF_HEADER 64

/// The sink that appends to such a buffer. `oom` is set when it ran out of memory.
typedef struct {
  GTEXT_Allocator alloc;
  unsigned char * base;
  size_t length;
  size_t capacity;
  bool oom;
} GTEXT_Encoding_Buffer;

/// The sink function that appends to a GTEXT_Encoding_Buffer; false (and `oom` set) if the buffer cannot grow.
bool gtext_encoding_buffer_write(void * user, const char * bytes, size_t len);

/// Copy the options' allocator (or the default) into `out`.
void gtext_encoding_allocator_copy(GTEXT_Allocator * out, const GTEXT_Allocator * given);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_ENCODING_ENCODING_INTERNAL_H
