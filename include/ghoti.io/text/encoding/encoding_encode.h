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
 * @file encoding_encode.h
 * @brief The standard's encoders: UTF-8 to the bytes of an encoding.
 *
 * A GTEXT_Encoder takes UTF-8 in pieces of any size and writes the bytes of a
 * legacy or Unicode encoding to a sink, with the standard's behaviour for a code
 * point the encoding cannot represent: HTML's numeric character reference
 * (`&#8364;`, the standard's "html" error mode, which is what forms and the
 * HTML serializer use), a status (the standard's "fatal", which URL
 * percent-encoding uses), or a replacement.
 *
 * **Input is UTF-8, not scalar values.** The standard's encoders are defined on
 * scalar values, "effectively I/O queues of scalar values, i.e., they contain no
 * surrogates". UTF-8 that is not well-formed (a stray byte, an overlong form, an
 * encoded surrogate, a truncated sequence) is not a sequence of scalar values, and is
 * handled the way the Encoding API handles a JavaScript string that is not: it
 * becomes U+FFFD (by the same maximal-subpart rule as the decoder), and then
 * that is encoded or not like any other. In fatal mode it is GTEXT_ENCODING_E_MALFORMED
 * instead. A cut between the bytes of a sequence is not an error.
 *
 * **Which encodings have an encoder.** The UTF-8 encoder and the single-byte
 * encodings' (and `x-user-defined`'s). `replacement`, UTF-16BE and UTF-16LE have none
 * (GTEXT_ENCODING_E_NO_ENCODER): "get an output encoding" gives UTF-8 for them
 * (gtext_encoding_output_encoding()). The multi-byte encodings are
 * GTEXT_ENCODING_E_UNSUPPORTED until the next stage.
 *
 * End of input is a call with `flush` set, as for the decoder; after any non-OK
 * status the encoder is back at its starting state.
 */

#ifndef GHOTI_IO_GTEXT_ENCODING_ENCODING_ENCODE_H
#define GHOTI_IO_GTEXT_ENCODING_ENCODING_ENCODE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/encoding/encoding_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_Encoder_Error_Mode
 * @brief What an encoder does with a code point its encoding cannot represent.
 */
typedef enum {
  GTEXT_ENCODER_FATAL = 0, ///< Stop with GTEXT_ENCODING_E_UNENCODABLE (the standard's "fatal"). The default, because
                           ///< the other two lose information without saying so.
  GTEXT_ENCODER_HTML,      ///< Write `&#` and the code point in decimal and `;` (the standard's "html"): what HTML forms
                           ///< do. It cannot be told from text that said that, so it can lose data silently.
  GTEXT_ENCODER_REPLACEMENT ///< Write `?` (0x3F). **Not a mode of the standard**, whose encoders are only ever run in "html" or
                           ///< "fatal" mode; the replacement every other encoding API offers, and what the standard's
                           ///< encoders for these ASCII-compatible encodings can always say.
} GTEXT_Encoder_Error_Mode;

/**
 * @struct GTEXT_Encoder_Options
 * @brief What an encoder may do. Zeroed is the same as the defaults.
 *
 * The struct is part of one version of the library (as every options struct here is, and
 * the library's namespace changes with the major version), so a field may be added when the version changes; there is no size field.
 */
typedef struct {
  /** The allocator for the encoder and for the buffer of gtext_encoding_encode_buffer(); NULL for
   *  gtext_allocator_default(). The struct is copied; the `ctx` it holds must outlive the encoder. */
  const GTEXT_Allocator * allocator;

  /** What to do with a code point the encoding cannot represent. Default GTEXT_ENCODER_FATAL. */
  GTEXT_Encoder_Error_Mode error_mode;

  /** Most bytes of output for one stream (from the start, or the last flush or failure), or 0 for the default of
   *  256 MiB; SIZE_MAX removes the cap in effect. A flush ends a stream and starts the count again. Exceeding it is GTEXT_ENCODING_E_LIMIT_OUTPUT, and the sink has then received a prefix of the
   *  output, never more than the limit and never part of an encoded character or a reference. */
  size_t max_output_bytes;
} GTEXT_Encoder_Options;

/**
 * @brief The defaults, spelled out.
 *
 * @return The defaults, by value: the default allocator, fatal mode, 256 MiB of output.
 */
GTEXT_API GTEXT_Encoder_Options gtext_encoder_options_default(void);

/**
 * @struct GTEXT_Encoder_Result
 * @brief What an encode call reports besides the status.
 */
typedef struct {
  GTEXT_Encoding_Status status; ///< What the call returned.
  const char * message;         ///< A static description of the status; never freed; never NULL after a call.
  size_t input_bytes;           ///< Bytes of the call's input that were taken: all of them on success; on a failure those up to and
                                ///< including the byte at which it was found.
  size_t output_bytes;          ///< Bytes the sink was given during this call.
  size_t replacements;          ///< Code points written as a reference or `?`, and malformed input written as U+FFFD, during this call.
  size_t error_offset;          ///< On E_UNENCODABLE or E_MALFORMED: the offset, from the start of the stream, of the first byte of the
                                ///< character (E_UNENCODABLE) or of the byte at which the malformation was found (E_MALFORMED). 0 otherwise.
  uint32_t error_code_point;    ///< On E_UNENCODABLE: the code point. 0 otherwise.
  size_t steps;                 ///< Handler steps taken during this call (the UTF-8 reader's and the encoding's): the work, which is at most
                                ///< three per byte of input.
} GTEXT_Encoder_Result;

/**
 * A streaming encoder. Opaque.
 *
 * Not safe to use from two threads at once; any number of encoders can be used at once, one per thread.
 */
typedef struct GTEXT_Encoder GTEXT_Encoder;

/**
 * @brief Make an encoder.
 *
 * @param encoding The encoding to write. Apply gtext_encoding_output_encoding() first when it comes from a label.
 * @param opts     Options, or NULL for the defaults.
 * @param out      Receives the encoder. Written only on success; free it with gtext_encoder_free().
 * @return GTEXT_ENCODING_OK; E_INVALID (NULL `out`, `encoding` that is not an encoding); E_OPTIONS (an error mode that is
 *   none of the enum); E_NO_ENCODER (`replacement`, UTF-16BE, UTF-16LE); E_UNSUPPORTED (a multi-byte
 *   encoding); E_OOM.
 */
GTEXT_API GTEXT_Encoding_Status gtext_encoder_new(
    GTEXT_Encoding encoding, const GTEXT_Encoder_Options * opts, GTEXT_Encoder ** out);

/**
 * @brief Encode the next piece of the input.
 *
 * @param encoder The encoder.
 * @param in      The UTF-8; may be NULL if `len` is 0.
 * @param len     Its length.
 * @param flush   Whether this is the end of the input (an incomplete sequence held from before is then
 *                malformed input).
 * @param sink    Where the bytes go. Not NULL; its `write` not NULL.
 * @param result  Receives the outcome, or NULL.
 * @return GTEXT_ENCODING_OK; E_INVALID (the arguments); E_UNENCODABLE or E_MALFORMED (fatal mode);
 *   E_LIMIT_OUTPUT; E_WRITE; E_INTERNAL (a library defect). On any failure but E_INVALID (an argument error does not touch the stream)
 *   the sink has received the output produced before it, and the encoder is back at the start. `result->input_bytes` is the number
 *   of bytes of `in` up to and including the one at which a failure was found.
 */
GTEXT_API GTEXT_Encoding_Status gtext_encoder_encode(GTEXT_Encoder * encoder, const void * in,
    size_t len, bool flush, const GTEXT_Encoding_Sink * sink, GTEXT_Encoder_Result * result);

/**
 * @brief Abandon the stream in progress and start over, as after a flush.
 *
 * @param encoder The encoder. NULL is ignored.
 */
GTEXT_API void gtext_encoder_reset(GTEXT_Encoder * encoder);

/**
 * @brief The encoding the encoder writes.
 *
 * @param encoder The encoder.
 * @return The encoding, or GTEXT_ENCODING_NONE for NULL.
 */
GTEXT_API GTEXT_Encoding gtext_encoder_encoding(const GTEXT_Encoder * encoder);

/**
 * @brief Release an encoder.
 *
 * @param encoder The encoder. NULL is ignored.
 */
GTEXT_API void gtext_encoder_free(GTEXT_Encoder * encoder);

/**
 * @brief Encode a whole input to a new buffer.
 *
 * @param encoding As for gtext_encoder_new().
 * @param opts     Options, or NULL for the defaults.
 * @param in       The UTF-8; may be NULL if `len` is 0.
 * @param len      Its length.
 * @param out      Receives the bytes, followed by one NUL byte not counted in `out_len`. Release with
 *                 gtext_encoding_buffer_free(). Written only on success.
 * @param out_len  Receives the length. May be NULL. Untouched on failure.
 * @param result   Receives the outcome, or NULL.
 * @return As gtext_encoder_new() and gtext_encoder_encode(); a failure leaves no buffer.
 */
GTEXT_API GTEXT_Encoding_Status gtext_encoding_encode_buffer(GTEXT_Encoding encoding,
    const GTEXT_Encoder_Options * opts, const void * in, size_t len, char ** out, size_t * out_len,
    GTEXT_Encoder_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ENCODING_ENCODING_ENCODE_H
