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
 * @file encoding_decode.h
 * @brief The standard's decoders: bytes in an encoding to UTF-8.
 *
 * A GTEXT_Decoder reads the bytes of one encoding in pieces of any size and
 * hands UTF-8 to a sink as it goes. It is the standard's decoder algorithms
 * (section 8.1 for UTF-8, 9.1 for the single-byte encodings, 14 for the rest)
 * with its error handling: a byte sequence that is not well-formed becomes
 * U+FFFD, replacing the *maximal subpart* of the ill-formed sequence for UTF-8,
 * and never swallowing an ASCII byte; in fatal mode it is a status instead.
 *
 * **The output is identical however the input is cut.** A sequence split
 * between two calls is decoded as if it had not been, and the cut can fall
 * anywhere, including between the two bytes of a UTF-16 code unit and between
 * the first byte of a mark and the rest of it. This is tested at every cut
 * of short inputs and by 1-byte feeds of long ones.
 *
 * **End of input is a call with `flush` set** (the standard's
 * end-of-queue), which decodes what was held (a truncated sequence is one
 * U+FFFD) and returns the decoder to its starting state, so that one decoder
 * can read any number of streams. After any non-OK status **but E_INVALID** the decoder is in
 * its starting state as well, and the next call begins a new stream (a failed
 * decode does not poison it: the Encoding API's `TextDecoder` is specified the
 * same way, and the standard's web-platform-tests check it). **One exception, the standard's own:**
 * after a fatal error (E_MALFORMED) in a call that did not flush, an ISO-2022-JP decoder goes on in the
 * **mode** it was in (ASCII, Roman, katakana or JIS X 0208, as the last escape sequence selected: the standard's
 * "decoder output state"), because "this is the only decoder which does not clear internal state before throwing
 * in stream mode" (the web-platform-tests' own words; their cases are a mode of Roman and of katakana). Nothing
 * else of it survives: not a half-read escape sequence or a leading byte (the next stream does not begin by finishing
 * them), not the "output" flag that makes two escape sequences with nothing between an error; and its position, the
 * offsets it reports and everything else start over. A flush, gtext_decoder_reset(), a failure that is not an error in
 * the data (a limit, a refusing sink) and an error in a call that did flush abandon the stream like any other,
 * mode included.
 *
 * **The multi-byte encodings** (GBK, gb18030, Big5, EUC-JP, ISO-2022-JP, Shift_JIS, EUC-KR) are the
 * standard's decoders, whose error handling puts a byte back where the standard says "restore byte to
 * ioQueue": an invalid pair does not swallow an ASCII trail byte, and a four-byte form of gb18030
 * broken at its last byte gives back the three bytes after the first. Two code points that one pair of
 * bytes decodes to (Big5's four sequences) are written together or not at all, so an output limit
 * never falls between them.
 *
 * **Byte order marks.** The three variants of the standard are two options:
 * `bom_sniffing` is "decode" (a UTF-8 or UTF-16 mark at the start chooses the
 * encoding, whatever was asked for, and is consumed), `bom_removal` without it is
 * "UTF-8 decode" (a U+FEFF at the start of the output of a UTF-8, UTF-16LE or
 * UTF-16BE decoder is dropped, which is what the Encoding API does unless
 * `ignoreBOM` is set), and neither is "UTF-8 decode without BOM". With
 * `bom_sniffing` the other option has no effect: the sniff already takes the mark.
 *
 * The streaming calls allocate nothing (gtext_decoder_new() does, and so does the
 * one-shot buffer form, for its output), and the work is linear in the
 * input: every byte is examined a bounded number of times, and there is no
 * recursion.
 */

#ifndef GHOTI_IO_GTEXT_ENCODING_ENCODING_DECODE_H
#define GHOTI_IO_GTEXT_ENCODING_ENCODING_DECODE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/encoding/encoding_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_Decoder_Error_Mode
 * @brief What a decoder does with a sequence that is not well-formed.
 */
typedef enum {
  GTEXT_DECODER_REPLACEMENT = 0, ///< Write U+FFFD for it, as the standard's "replacement" error mode. The default.
  GTEXT_DECODER_FATAL            ///< Stop with GTEXT_ENCODING_E_MALFORMED (the standard's "fatal").
} GTEXT_Decoder_Error_Mode;

/**
 * @struct GTEXT_Decoder_Options
 * @brief What a decoder may do. Zeroed is the same as the defaults.
 *
 * The struct is part of one version of the library (as every options struct here is, and
 * the library's namespace changes with the major version), so a field may be added when the version changes; there is no size field.
 */
typedef struct {
  /** The allocator for the decoder and for the buffer of gtext_encoding_decode_buffer(); NULL for
   *  gtext_allocator_default(). The struct is copied; the `ctx` it holds must outlive the decoder. */
  const GTEXT_Allocator * allocator;

  /** What to do with ill-formed input. Default GTEXT_DECODER_REPLACEMENT. */
  GTEXT_Decoder_Error_Mode error_mode;

  /** The standard's "decode": a byte order mark chooses the encoding (UTF-8, UTF-16BE or UTF-16LE) over the
   *  one asked for, and is not part of the output. Default false. */
  bool bom_sniffing;

  /** Drop a U+FEFF that is the first thing a UTF-8, UTF-16LE or UTF-16BE decoder writes ("UTF-8 decode"; the
   *  Encoding API's `ignoreBOM` false). No effect with `bom_sniffing` or for other encodings. Default false. */
  bool bom_removal;

  /** Most bytes of output for one stream (from the start, or the last flush or failure), or 0 for the default of
   *  256 MiB; SIZE_MAX removes the cap in effect. A flush ends a stream and starts the count again. Exceeding it is GTEXT_ENCODING_E_LIMIT_OUTPUT, and the sink has then received a prefix of the
   *  output, never more than the limit and never part of a code point. */
  size_t max_output_bytes;
} GTEXT_Decoder_Options;

/**
 * @brief The defaults, spelled out.
 *
 * @return The defaults, by value: the default allocator, replacement mode, no mark handling, 256 MiB of output.
 */
GTEXT_API GTEXT_Decoder_Options gtext_decoder_options_default(void);

/**
 * @struct GTEXT_Decoder_Result
 * @brief What a decode call reports besides the status.
 *
 * Filled by every call that gets as far as its arguments, whether or not it
 * succeeded. Holds nothing that is freed.
 */
typedef struct {
  GTEXT_Encoding_Status status; ///< What the call returned.
  const char * message;         ///< A static description of the status; never freed; never NULL after a call.
  GTEXT_Encoding encoding;      ///< The encoding the call decoded as: the one asked for, or the one a byte order mark chose (once
                                ///< `bom_sniffing` has decided; before then, the one asked for).
  size_t input_bytes;           ///< Bytes of the call's input that were taken: all of them on success; on a failure those up to and
                                ///< including the byte at which it was found.
  size_t output_bytes;          ///< Bytes the sink was given during this call.
  size_t replacements;          ///< U+FFFD written for ill-formed input during this call (replacement mode).
  size_t error_offset;          ///< On GTEXT_ENCODING_E_MALFORMED: the offset, from the start of the stream (the first byte after the last
                                ///< flush or failure), of the byte at which the error was found, or the length of the stream if the
                                ///< input ended in the middle of a sequence. 0 otherwise.
  bool bom_found;               ///< `bom_sniffing` took a byte order mark during this call.
  size_t steps;                 ///< Handler steps taken during this call, restored bytes read again included: the work, which is at most
                                ///< four per byte of input (and none for the runs of ASCII that are copied whole).
} GTEXT_Decoder_Result;

/**
 * A streaming decoder. Opaque.
 *
 * Not safe to use from two threads at once; any number of decoders can be used at once, one per thread.
 */
typedef struct GTEXT_Decoder GTEXT_Decoder;

/**
 * @brief Make a decoder.
 *
 * @param encoding The encoding to decode (the one to fall back on, with `bom_sniffing`). Any of the standard's
 *   (the seven multi-byte legacy encodings included).
 * @param opts     Options, or NULL for the defaults.
 * @param out      Receives the decoder. Written only on success; free it with gtext_decoder_free().
 * @return GTEXT_ENCODING_OK; E_INVALID (NULL `out`, `encoding` that is not an encoding); E_OPTIONS (an error mode that is
 *   none of the enum); E_OOM.
 */
GTEXT_API GTEXT_Encoding_Status gtext_decoder_new(
    GTEXT_Encoding encoding, const GTEXT_Decoder_Options * opts, GTEXT_Decoder ** out);

/**
 * @brief Decode the next piece of the input.
 *
 * Output goes to the sink as it is produced. With `flush` false the decoder keeps
 * what it must (an incomplete sequence, the first bytes of a possible mark) for
 * the next call; with it true the input has ended: the call decodes what is held
 * (an incomplete sequence is an error) and the decoder starts over.
 *
 * @param decoder The decoder.
 * @param in      The bytes; may be NULL if `len` is 0.
 * @param len     Their number.
 * @param flush   Whether this is the end of the input.
 * @param sink    Where the UTF-8 goes. Not NULL; its `write` not NULL.
 * @param result  Receives the outcome, or NULL.
 * @return GTEXT_ENCODING_OK; E_INVALID (the arguments); E_MALFORMED (fatal mode); E_LIMIT_OUTPUT;
 *   E_WRITE (the sink returned false); E_INTERNAL (a library defect). On any failure but E_INVALID (an argument error does not touch
 *   the stream) the sink has received the output produced before it, and the decoder is back at the start. `result->input_bytes`
 *   is the number of bytes of `in` up to and including the one at which a failure was found.
 */
GTEXT_API GTEXT_Encoding_Status gtext_decoder_decode(GTEXT_Decoder * decoder, const void * in,
    size_t len, bool flush, const GTEXT_Encoding_Sink * sink, GTEXT_Decoder_Result * result);

/**
 * @brief Abandon the stream in progress and start over, as after a flush.
 *
 * @param decoder The decoder. NULL is ignored.
 */
GTEXT_API void gtext_decoder_reset(GTEXT_Decoder * decoder);

/**
 * @brief The encoding the decoder is decoding as.
 *
 * The one asked for, except between the call in which `bom_sniffing` took a mark and the next flush,
 * when it is the one the mark chose.
 *
 * @param decoder The decoder.
 * @return The encoding, or GTEXT_ENCODING_NONE for NULL.
 */
GTEXT_API GTEXT_Encoding gtext_decoder_encoding(const GTEXT_Decoder * decoder);

/**
 * @brief Release a decoder.
 *
 * @param decoder The decoder. NULL is ignored.
 */
GTEXT_API void gtext_decoder_free(GTEXT_Decoder * decoder);

/**
 * @brief Decode a whole input to a new buffer.
 *
 * The one-shot form: a decoder fed `in` once with `flush` set.
 *
 * @param encoding As for gtext_decoder_new().
 * @param opts     Options, or NULL for the defaults.
 * @param in       The bytes; may be NULL if `len` is 0.
 * @param len      Their number.
 * @param out      Receives the UTF-8, followed by one NUL byte not counted in `out_len` (the output may
 *                 hold NULs of its own: use `out_len`). Release with gtext_encoding_buffer_free(). Written
 *                 only on success.
 * @param out_len  Receives the length. May be NULL. Untouched on failure.
 * @param result   Receives the outcome, or NULL.
 * @return As gtext_decoder_new() and gtext_decoder_decode(); a failure leaves no buffer.
 */
GTEXT_API GTEXT_Encoding_Status gtext_encoding_decode_buffer(GTEXT_Encoding encoding,
    const GTEXT_Decoder_Options * opts, const void * in, size_t len, char ** out, size_t * out_len,
    GTEXT_Decoder_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ENCODING_ENCODING_DECODE_H
