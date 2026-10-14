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
 * The multi-byte stage adds rows and handlers (encoding_cjk.c) and their index tables (tables/encoding_tables_cjk.c).
 * It does not touch the public headers or the callers. Inside, the drivers learned three small things: a decoder
 * step may emit two code points that are written together or not at all, an encoder step may ask for a different
 * code point than the one it was given to be handled again, and may name the code point an error is "with"
 * (ISO-2022-JP needs both); the pushback and the idle test were already what the multi-byte decoders need.
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

/* ---- the multi-byte indexes (tables/encoding_tables_cjk.c) ----------------------------- */

/// How many pointers each forward array holds (the generator checks that this is what it wrote; the generated file includes this header, so a
/// different count is also a compile error).
#define GTEXT_JIS0208_POINTERS 11280
#define GTEXT_JIS0212_POINTERS 8836
#define GTEXT_GB18030_POINTERS 23940
#define GTEXT_BIG5_POINTERS 19782
#define GTEXT_EUC_KR_POINTERS 23940
#define GTEXT_ISO_2022_JP_KATAKANA_POINTERS 63
/// Bytes of the bit array that says which Big5 pointers map to plane 2.
#define GTEXT_BIG5_PLANE2_BYTES ((GTEXT_BIG5_POINTERS + 7) / 8)
/// Rows of index gb18030 ranges.
#define GTEXT_GB18030_RANGES 207
/// A forward array's entry (and a reverse table's) for "none".
#define GTEXT_INDEX_NONE 0xFFFFu

/// index jis0208, jis0212, gb18030, EUC-KR and Big5 (low 16 bits; see gtext_encoding_big5_plane2): pointer to code point, 0xFFFF for none.
extern const uint16_t gtext_encoding_jis0208_decode[GTEXT_JIS0208_POINTERS];
extern const uint16_t gtext_encoding_jis0212_decode[GTEXT_JIS0212_POINTERS];
extern const uint16_t gtext_encoding_gb18030_decode[GTEXT_GB18030_POINTERS];
extern const uint16_t gtext_encoding_euc_kr_decode[GTEXT_EUC_KR_POINTERS];
extern const uint16_t gtext_encoding_big5_decode[GTEXT_BIG5_POINTERS];
/// Bit `pointer` of the Big5 index is set when its code point is 0x20000 plus the entry of gtext_encoding_big5_decode.
extern const uint8_t gtext_encoding_big5_plane2[GTEXT_BIG5_PLANE2_BYTES];
/// index ISO-2022-JP katakana: halfwidth U+FF61 + pointer to the code point the encoder writes instead.
extern const uint16_t gtext_encoding_iso_2022_jp_katakana_decode[GTEXT_ISO_2022_JP_KATAKANA_POINTERS];
/// index gb18030 ranges: the rows' pointers and code points, both ascending.
extern const uint32_t gtext_encoding_gb18030_ranges_pointer[GTEXT_GB18030_RANGES];
extern const uint32_t gtext_encoding_gb18030_ranges_cp[GTEXT_GB18030_RANGES];

/// A reverse table, code point to pointer: the BMP in blocks of 1 << shift pointers (`index` says which block of `pool` a range of code points
/// uses; the block 0 is empty), the code points above it in two sorted arrays. 0xFFFF is "none".
typedef struct {
  const uint16_t * index;
  const uint16_t * pool;
  uint32_t limit;           ///< Code points of the BMP at or above this have no pointer.
  uint8_t shift;
  const uint32_t * sup_cp;  ///< Ascending code points above the BMP that have a pointer, or NULL.
  const uint16_t * sup_ptr; ///< ... and the pointer of each.
  uint32_t sup_count;
} GTEXT_Rev_Index;

/// "The index pointer for codePoint in index" of jis0208 (the first pointer), of "index Shift_JIS pointer" (the first pointer of jis0208 without
/// the entries 8272 to 8835), of "index Big5 pointer" (the first pointer of the entries from 5024 on, the last for six code points), of gb18030
/// and of EUC-KR (the first).
extern const GTEXT_Rev_Index gtext_encoding_jis0208_rev;
extern const GTEXT_Rev_Index gtext_encoding_shift_jis_rev;
extern const GTEXT_Rev_Index gtext_encoding_big5_rev;
extern const GTEXT_Rev_Index gtext_encoding_gb18030_rev;
extern const GTEXT_Rev_Index gtext_encoding_euc_kr_rev;

/// The pointer of a code point in a reverse table, or GTEXT_INDEX_NONE.
static inline uint32_t gtext_rev_lookup(const GTEXT_Rev_Index * r, uint32_t cp) {
  if (cp < 0x10000) {
    if (cp >= r->limit) {
      return GTEXT_INDEX_NONE;
    }
    const uint32_t block = r->index[cp >> r->shift];
    return r->pool[(block << r->shift) + (cp & ((1u << r->shift) - 1))];
  }
  size_t lo = 0;
  size_t hi = r->sup_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (r->sup_cp[mid] == cp) {
      return r->sup_ptr[mid];
    }
    if (r->sup_cp[mid] < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return GTEXT_INDEX_NONE;
}

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
  bool skip;              ///< CONTINUE: the bytes read so far make no code point and never will (an ISO-2022-JP escape sequence), so the
                          ///< character after them begins after them. Only the source map reads it.
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

/// gb18030's and GBK's decoder (section 10.2.1): "gb18030 first", "second" and "third", each 0x00 for none.
typedef struct {
  uint8_t first;
  uint8_t second;
  uint8_t third;
} GTEXT_Gb18030_State;

/// The decoders with one leading byte (Big5, Shift_JIS, EUC-KR: section 11.1.1, 12.3.1, 13.1.1) and EUC-JP's (12.1.1, which adds "jis0212").
typedef struct {
  uint8_t leading;
  bool jis0212;
} GTEXT_Lead_State;

/// ISO-2022-JP's decoder (section 12.2.1): the decoder state, the output state, the leading byte and "output".
typedef struct {
  uint8_t state;
  uint8_t output_state;
  uint8_t leading;
  bool output;
} GTEXT_Iso2022jp_State;

/// A decoder's state. The union grows with the encodings; the driver treats it as opaque.
typedef union {
  GTEXT_Utf8_State utf8;
  GTEXT_Utf16_State utf16;
  GTEXT_Gb18030_State gb18030;
  GTEXT_Lead_State lead;
  GTEXT_Iso2022jp_State iso2022jp;
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
/// The decoder of a multi-byte encoding (encoding_cjk.c), or NULL for any other.
const GTEXT_Dec_Codec * gtext_encoding_cjk_dec_codec(GTEXT_Encoding encoding);
/// ISO-2022-JP's decoder after a fatal error in a call that did not flush: the mode survives, nothing else (encoding_cjk.c).
void gtext_encoding_iso_2022_jp_dec_resume(GTEXT_Dec_State * state);

/* ---- encoders ------------------------------------------------------------------------- */

typedef enum {
  GTEXT_ENC_OK = 0,
  GTEXT_ENC_ERROR,     ///< The code point is unencodable.
  GTEXT_ENC_FINISHED
} GTEXT_Enc_Result;

typedef struct {
  uint8_t bytes[8];
  uint8_t n;
  bool restore;        ///< Handle a code point again after writing `bytes`: `restore_cp`, which is the one handled unless the handler changed it
                       ///< ("restore codePoint to ioQueue" after "set it to"; ISO-2022-JP).
  uint32_t restore_cp; ///< preset by the driver to the code point handled; a handler changes it only when the standard sets codePoint before restoring it
  uint32_t error_cp;   ///< ERROR: the code point the error is "with", which the HTML mode writes as a reference; preset to the code point handled (ISO-2022-JP returns U+FFFD for U+000E, U+000F and U+001B). The fatal status carries the code point handled.
} GTEXT_Enc_Step;

typedef union {
  struct {                    ///< A single-byte encoder: its index, sorted by code point.
    const uint16_t * cps;
    const uint8_t * bytes;
    size_t count;
  } sb;
  uint32_t reserved[4];       ///< The state of the multi-byte encoders: ISO-2022-JP keeps its mode in reserved[0]; the others have none.
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
/// The encoder of a multi-byte encoding (encoding_cjk.c), or NULL for any other.
const GTEXT_Enc_Codec * gtext_encoding_cjk_enc_codec(GTEXT_Encoding encoding);

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
#define GTEXT_ENCODING_CHUNK GTEXT_ENCODING_MAX_CHUNK

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
  /* The source map of a decoder that was asked for one (all NULL and zero otherwise, which is every encoder): for each byte of `buf` the offset of the
     input byte that began the code point it belongs to and whether that code point is a U+FFFD for ill-formed input. `cur_off` and `cur_rep` describe the
     code point about to be put; a run of ASCII (gtext_out_put_run) is linear from `cur_off`. */
  const GTEXT_Encoding_Mapped_Sink * msink; ///< Where a mapped chunk goes (instead of `sink`), or NULL.
  size_t * off;                 ///< GTEXT_ENCODING_CHUNK offsets, or NULL.
  unsigned char * rep;          ///< GTEXT_ENCODING_CHUNK flags, or NULL.
  unsigned char * wid;          ///< GTEXT_ENCODING_CHUNK widths (bytes of the input of each code point), or NULL.
  size_t cur_off;
  unsigned char cur_rep;
  unsigned char cur_w;
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
  if (out->off) {
    for (size_t k = 0; k < n; k++) {
      out->off[out->n + k] = out->cur_off;
      out->rep[out->n + k] = out->cur_rep;
      out->wid[out->n + k] = out->cur_w;
    }
  }
  out->n += n;
  out->total += n;
  return true;
}

/// Add a run of any length (the ASCII fast paths): the same, in pieces; the part that fits the limit is delivered before the failure.
/// `accepted` receives how many bytes were taken, so that the byte at which a limit or a sink failed is exact: it is the one after them.
bool gtext_out_put_run(GTEXT_Out * out, const unsigned char * bytes, size_t n, size_t * accepted);

/// The UTF-8 of a scalar value into `b` (at least 4 bytes); returns how many.
static inline size_t gtext_utf8_bytes(unsigned char * b, uint32_t cp) {
  if (cp < 0x80) {
    b[0] = (unsigned char)cp;
    return 1;
  }
  if (cp < 0x800) {
    b[0] = (unsigned char)(0xC0 | (cp >> 6));
    b[1] = (unsigned char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    b[0] = (unsigned char)(0xE0 | (cp >> 12));
    b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (unsigned char)(0x80 | (cp & 0x3F));
    return 3;
  }
  b[0] = (unsigned char)(0xF0 | (cp >> 18));
  b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
  b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
  b[3] = (unsigned char)(0x80 | (cp & 0x3F));
  return 4;
}

/// Add the UTF-8 of a scalar value.
static inline bool gtext_out_put_cp(GTEXT_Out * out, uint32_t cp) {
  unsigned char b[4];
  return gtext_out_put(out, b, gtext_utf8_bytes(b, cp));
}

/// Add the UTF-8 of two scalar values together: both or, when the limit or the sink refuses, neither (Big5's four two-code-point sequences).
static inline bool gtext_out_put_cp2(GTEXT_Out * out, uint32_t cp0, uint32_t cp1) {
  unsigned char b[8];
  const size_t n = gtext_utf8_bytes(b, cp0);
  return gtext_out_put(out, b, n + gtext_utf8_bytes(b + n, cp1));
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
