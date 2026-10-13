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
 * @file encoding_core.h
 * @brief The Encoding Standard's encodings, labels and statuses.
 *
 * This module implements the WHATWG Encoding Standard
 * (https://encoding.spec.whatwg.org/): the table of encodings and the labels
 * that name them, and (in encoding_decode.h and encoding_encode.h) the
 * standard's decoders and encoders. It is the layer HTML's encoding sniffing,
 * forms and serialization are defined against, and it stands alone: nothing in
 * it reads a locale or calls `iconv`.
 *
 * **What this release reads and writes.** UTF-8, UTF-16LE, UTF-16BE, the
 * `replacement` encoding, `x-user-defined` and all 28 single-byte encodings
 * (`ibm866`, the ISO-8859 family, `koi8-r`, `koi8-u`, `macintosh`,
 * `windows-874`, `windows-1250` to `windows-1258`, `x-mac-cyrillic`, and
 * `ISO-8859-8-I`). **The multi-byte legacy encodings** (`gbk`, `gb18030`,
 * `Big5`, `EUC-JP`, `ISO-2022-JP`, `Shift_JIS` and `EUC-KR`) have labels that
 * resolve and names that print, and **no codec yet**: asking for a decoder
 * or an encoder for one is GTEXT_ENCODING_E_UNSUPPORTED, which is the only
 * thing that changes when they arrive (gtext_encoding_is_supported() tells
 * the two apart in advance). Nothing else in the interface is affected.
 */

#ifndef GHOTI_IO_GTEXT_ENCODING_ENCODING_CORE_H
#define GHOTI_IO_GTEXT_ENCODING_ENCODING_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_Encoding
 * @brief The standard's 40 encodings, in the order of its table.
 *
 * `GTEXT_ENCODING_NONE` (0) is not an encoding: it is what an unknown label
 * resolves to, and the value of a zeroed field. The numeric values of the others
 * are not part of the interface (the order follows the standard's table, which
 * a revision of the standard may extend); `GTEXT_ENCODING_COUNT` is one more
 * than the largest, so that it can size an array indexed by encoding.
 */
typedef enum {
  GTEXT_ENCODING_NONE = 0, ///< Not an encoding (an unknown label).

  GTEXT_ENCODING_UTF_8,        ///< UTF-8. The only encoding new formats are to use.

  GTEXT_ENCODING_IBM866,       ///< ibm866 (DOS Cyrillic).
  GTEXT_ENCODING_ISO_8859_2,   ///< ISO-8859-2 (Latin 2).
  GTEXT_ENCODING_ISO_8859_3,   ///< ISO-8859-3 (Latin 3); seven bytes map to nothing.
  GTEXT_ENCODING_ISO_8859_4,   ///< ISO-8859-4 (Latin 4).
  GTEXT_ENCODING_ISO_8859_5,   ///< ISO-8859-5 (Cyrillic).
  GTEXT_ENCODING_ISO_8859_6,   ///< ISO-8859-6 (Arabic); the "-e" and "-i" labels are this too.
  GTEXT_ENCODING_ISO_8859_7,   ///< ISO-8859-7 (Greek).
  GTEXT_ENCODING_ISO_8859_8,   ///< ISO-8859-8 (Hebrew, visual order).
  GTEXT_ENCODING_ISO_8859_8_I, ///< ISO-8859-8-I (Hebrew, logical order); the same bytes as ISO-8859-8.
  GTEXT_ENCODING_ISO_8859_10,  ///< ISO-8859-10 (Latin 6).
  GTEXT_ENCODING_ISO_8859_13,  ///< ISO-8859-13 (Baltic).
  GTEXT_ENCODING_ISO_8859_14,  ///< ISO-8859-14 (Celtic).
  GTEXT_ENCODING_ISO_8859_15,  ///< ISO-8859-15 (Latin 9).
  GTEXT_ENCODING_ISO_8859_16,  ///< ISO-8859-16 (Latin 10).
  GTEXT_ENCODING_KOI8_R,       ///< KOI8-R.
  GTEXT_ENCODING_KOI8_U,       ///< KOI8-U.
  GTEXT_ENCODING_MACINTOSH,    ///< macintosh (Mac OS Roman).
  GTEXT_ENCODING_WINDOWS_874,  ///< windows-874 (Thai).
  GTEXT_ENCODING_WINDOWS_1250, ///< windows-1250 (Central European).
  GTEXT_ENCODING_WINDOWS_1251, ///< windows-1251 (Cyrillic).
  GTEXT_ENCODING_WINDOWS_1252, ///< windows-1252, which the labels `latin1`, `iso-8859-1` and `ascii` also name.
  GTEXT_ENCODING_WINDOWS_1253, ///< windows-1253 (Greek).
  GTEXT_ENCODING_WINDOWS_1254, ///< windows-1254 (Turkish).
  GTEXT_ENCODING_WINDOWS_1255, ///< windows-1255 (Hebrew).
  GTEXT_ENCODING_WINDOWS_1256, ///< windows-1256 (Arabic).
  GTEXT_ENCODING_WINDOWS_1257, ///< windows-1257 (Baltic).
  GTEXT_ENCODING_WINDOWS_1258, ///< windows-1258 (Vietnamese).
  GTEXT_ENCODING_X_MAC_CYRILLIC, ///< x-mac-cyrillic.

  GTEXT_ENCODING_GBK,          ///< GBK. No codec in this release.
  GTEXT_ENCODING_GB18030,      ///< gb18030. No codec in this release.
  GTEXT_ENCODING_BIG5,         ///< Big5. No codec in this release.
  GTEXT_ENCODING_EUC_JP,       ///< EUC-JP. No codec in this release.
  GTEXT_ENCODING_ISO_2022_JP,  ///< ISO-2022-JP. No codec in this release.
  GTEXT_ENCODING_SHIFT_JIS,    ///< Shift_JIS. No codec in this release.
  GTEXT_ENCODING_EUC_KR,       ///< EUC-KR. No codec in this release.

  GTEXT_ENCODING_REPLACEMENT,  ///< replacement: what `hz-gb-2312`, `iso-2022-kr` and the like decode as (one U+FFFD).
  GTEXT_ENCODING_UTF_16BE,     ///< UTF-16BE. A decoder only.
  GTEXT_ENCODING_UTF_16LE,     ///< UTF-16LE, which the label `utf-16` also names. A decoder only.
  GTEXT_ENCODING_X_USER_DEFINED, ///< x-user-defined: 0x80 to 0xFF are U+F780 to U+F7FF.

  GTEXT_ENCODING_COUNT         ///< One more than the largest encoding; not an encoding.
} GTEXT_Encoding;

/**
 * @enum GTEXT_Encoding_Status
 * @brief What the decoders and encoders report.
 *
 * A damaged input is not a failure in the standard's default mode (it becomes
 * U+FFFD, or a numeric character reference, and is counted); it is
 * GTEXT_ENCODING_E_MALFORMED or GTEXT_ENCODING_E_UNENCODABLE only in the
 * fatal modes.
 */
typedef enum {
  GTEXT_ENCODING_OK = 0,      ///< Success.
  GTEXT_ENCODING_E_INVALID,   ///< An argument was unusable (NULL data with a length, NULL sink or
                              ///< output, an encoding that is `GTEXT_ENCODING_NONE` or out of range).
  GTEXT_ENCODING_E_OOM,       ///< Out of memory. Nothing is leaked.
  GTEXT_ENCODING_E_OPTIONS,   ///< An option value out of range (an error mode that is none of the enum).
  GTEXT_ENCODING_E_UNSUPPORTED, ///< The encoding is one of the standard's but this release has no codec
                              ///< for it (the multi-byte encodings; see gtext_encoding_is_supported()).
  GTEXT_ENCODING_E_NO_ENCODER,  ///< The encoding has no encoder: `replacement`, UTF-16BE and UTF-16LE.
                              ///< The standard encodes for these as UTF-8; see gtext_encoding_output_encoding().
  GTEXT_ENCODING_E_MALFORMED, ///< Fatal mode: the input is not well-formed in the encoding (for a decoder),
                              ///< or not well-formed UTF-8 (for an encoder).
  GTEXT_ENCODING_E_UNENCODABLE, ///< Fatal mode: a code point the encoding cannot represent.
  GTEXT_ENCODING_E_LIMIT_OUTPUT, ///< More than `max_output_bytes` of output. The sink has received a
                              ///< prefix of it, never more than the limit.
  GTEXT_ENCODING_E_WRITE,     ///< The sink refused output (its function returned false).
  GTEXT_ENCODING_E_INTERNAL   ///< A defect of this library: a codec asked for more than the drivers can give it. Never content; please report it.
} GTEXT_Encoding_Status;

/**
 * @brief Where output goes: a function called with each chunk.
 *
 * Chunks are of up to a few kilobytes, cut only between code points (a
 * decoder's chunk is whole UTF-8 sequences; an encoder's, whole encoded
 * characters). The bytes are not terminated and are valid only for the call.
 *
 * @param user  The sink's `user`.
 * @param bytes The chunk; not NUL-terminated.
 * @param len   Its length, more than 0.
 * @return true to continue; false to stop, which the call reports as
 *   GTEXT_ENCODING_E_WRITE (the sink then receives nothing more).
 */
typedef bool (*GTEXT_Encoding_Write_Function)(void * user, const char * bytes, size_t len);

/**
 * @struct GTEXT_Encoding_Sink
 * @brief Where decoded or encoded bytes go.
 */
typedef struct {
  GTEXT_Encoding_Write_Function write; ///< Called for each chunk. Not NULL.
  void * user;                         ///< Passed back unchanged.
} GTEXT_Encoding_Sink;

/**
 * @brief A static, human-readable name for a status.
 *
 * Every status has a name of its own, and no two are alike.
 *
 * @param status Any value; an unknown one returns "unknown status".
 * @return A string that is never freed.
 */
GTEXT_API const char * gtext_encoding_status_string(GTEXT_Encoding_Status status);

/**
 * @brief The encoding a label names (the standard's "get an encoding").
 *
 * Leading and trailing ASCII white space (U+0009, U+000A, U+000C, U+000D and
 * U+0020, not U+000B and not the Unicode spaces) is removed, and the rest is
 * matched against the standard's table of 228 labels without regard to ASCII
 * case. Nothing else is accepted: not an IANA name that is not on the list,
 * not a label with an embedded NUL, not the empty string.
 *
 * **A label for `replacement` returns GTEXT_ENCODING_REPLACEMENT.** The Encoding
 * API's `TextDecoder` refuses that (it throws), but the standard's algorithm
 * resolves it, and HTML needs it to: a `<meta charset=hz-gb-2312>` is an
 * ordinary, resolvable label whose decoder yields one U+FFFD.
 *
 * @param label The label; not necessarily NUL-terminated. NULL is an unknown label.
 * @param len   Its length in bytes.
 * @return The encoding, or GTEXT_ENCODING_NONE for an unknown label.
 */
GTEXT_API GTEXT_Encoding gtext_encoding_for_label(const char * label, size_t len);

/**
 * @brief The standard's name for an encoding.
 *
 * "UTF-8", "windows-1252", "Shift_JIS", "gb18030", "x-user-defined" and so on:
 * the spelling of the table in section 4.2. Lowercasing it (ASCII) gives the
 * label `TextDecoder.encoding` reports.
 *
 * @param encoding Any value.
 * @return A string that is never freed, or NULL for GTEXT_ENCODING_NONE and for a value that is
 *   not an encoding.
 */
GTEXT_API const char * gtext_encoding_name(GTEXT_Encoding encoding);

/**
 * @brief Whether bytes 0x00 to 0x7F always mean U+0000 to U+007F.
 *
 * False for UTF-16BE, UTF-16LE, ISO-2022-JP and `replacement`; true for
 * every other encoding, `x-user-defined` included. (ISO-2022-JP maps the bytes
 * of its ASCII state that way but not in its other states, which an escape
 * sequence switches into; HTML's definition excludes it for that reason.)
 *
 * @param encoding Any value; false for anything that is not an encoding.
 */
GTEXT_API bool gtext_encoding_is_ascii_compatible(GTEXT_Encoding encoding);

/**
 * @brief The encoding to encode with when the standard says "get an output encoding".
 *
 * `replacement`, UTF-16BE and UTF-16LE have no encoder and encode as UTF-8
 * (URL parsing and form submission use this); every other encoding is its own.
 *
 * @param encoding Any value.
 * @return The output encoding, or GTEXT_ENCODING_NONE for anything that is not an encoding.
 */
GTEXT_API GTEXT_Encoding gtext_encoding_output_encoding(GTEXT_Encoding encoding);

/**
 * @brief Whether this release has a decoder for the encoding.
 *
 * True for everything but the seven multi-byte legacy encodings (GBK,
 * gb18030, Big5, EUC-JP, ISO-2022-JP, Shift_JIS, EUC-KR), whose decoders and
 * encoders arrive with the next stage. An encoder exists for the supported
 * encodings except `replacement`, UTF-16BE and UTF-16LE.
 *
 * @param encoding Any value; false for anything that is not an encoding.
 */
GTEXT_API bool gtext_encoding_is_supported(GTEXT_Encoding encoding);

/**
 * @brief Whether this release has an encoder for the encoding.
 *
 * False for `replacement`, UTF-16BE and UTF-16LE (the standard gives them none: encode with gtext_encoding_output_encoding(), which is UTF-8), for
 * the multi-byte legacy encodings until the next stage, and for anything that is not an encoding. gtext_encoder_new() says
 * the same with GTEXT_ENCODING_E_NO_ENCODER or GTEXT_ENCODING_E_UNSUPPORTED.
 *
 * @param encoding Any value.
 */
GTEXT_API bool gtext_encoding_has_encoder(GTEXT_Encoding encoding);

/**
 * @brief How many labels the table holds (228 for the standard as pinned).
 */
GTEXT_API size_t gtext_encoding_label_count(void);

/**
 * @brief One label of the table, in ascending byte order.
 *
 * For listing the table (and for testing it): the labels are lowercase ASCII.
 *
 * @param index    0 to gtext_encoding_label_count() - 1.
 * @param label    Receives a pointer to the label, which is static and is **not**
 *                 NUL-terminated (use `len`). May be NULL.
 * @param len      Receives the label's length. May be NULL.
 * @param encoding Receives the encoding the label names. May be NULL.
 * @return false (and writes nothing) if `index` is out of range.
 */
GTEXT_API bool gtext_encoding_label_at(
    size_t index, const char ** label, size_t * len, GTEXT_Encoding * encoding);

/**
 * @brief Release a buffer from gtext_encoding_decode_buffer() or gtext_encoding_encode_buffer().
 *
 * The buffer is released with the allocator it was made with, which it carries in a header of its own.
 * (Do not pass it to free(), and do not use the memory before it.)
 *
 * @param buffer The buffer. NULL is ignored.
 */
GTEXT_API void gtext_encoding_buffer_free(char * buffer);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ENCODING_ENCODING_CORE_H
