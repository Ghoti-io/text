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
 * The encoding sniffing algorithm of the HTML standard (13.2.3.2, "Determining the character encoding"): the pieces the tree builder, the
 * parsers and the public sniff function share. Internal.
 */

#ifndef GHOTI_IO_GTEXT_SRC_HTML_HTML_SNIFF_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_HTML_HTML_SNIFF_INTERNAL_H

#include <ghoti.io/text/encoding.h>
#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The bytes the prescan looks at: "the first 1024 bytes" that the standard's authoring requirement limits a declaration to.
#define GTEXT_HTML_PRESCAN_BYTES ((size_t)1024)

/// Is `s[0..n)` an ASCII case-insensitive match for `lower`, a NUL-terminated string of lower case ASCII?
bool html_ascii_equals_nocase(const char * s, size_t n, const char * lower);

/// The standard's "algorithm for extracting a character encoding from a meta element" (2.5.3): the encoding the `content` attribute's value
/// declares, or GTEXT_ENCODING_NONE for "nothing" (and for a label that names no encoding).
GTEXT_Encoding html_meta_extract_encoding(const char * s, size_t n);

/// The standard's "BOM sniff" over the first `n` bytes (`n` at least 3, or the whole input if it is shorter): the encoding of a byte order mark and
/// its length, or GTEXT_ENCODING_NONE.
GTEXT_Encoding html_bom_sniff(const unsigned char * d, size_t n, size_t * length);

/// The standard's "prescan a byte stream to determine its encoding" over `d[0..n)`, `n` at most GTEXT_HTML_PRESCAN_BYTES (the end condition), with its
/// fallback: when it runs out of bytes without an answer, "get an XML encoding". GTEXT_ENCODING_NONE for failure. UTF-16 results are UTF-8 as the
/// standard says; `x-user-defined` is `windows-1252`.
GTEXT_Encoding html_prescan(const unsigned char * d, size_t n);

/// html_prescan() and, in `*work` if it is not NULL, the units of work it did (positions advanced, bytes scanned for the end of a comment or a
/// tag, the attribute names compared), the deterministic measure the quadratic-nesting suite (tests/test-quadratic.cpp) holds to a bound: it is
/// at most a constant multiple of GTEXT_HTML_PRESCAN_BYTES however long the input is.
GTEXT_Encoding html_prescan_counted(const unsigned char * d, size_t n, size_t * work);

/// The standard's "get an XML encoding" alone, over `d[0..n)`.
GTEXT_Encoding html_get_xml_encoding(const unsigned char * d, size_t n);

/// The encoding a NUL-terminated option label names, or GTEXT_ENCODING_NONE (NULL, and what no label names).
GTEXT_Encoding html_label_encoding(const char * label);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_HTML_HTML_SNIFF_INTERNAL_H
