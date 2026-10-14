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
 * What the files of the public HTML API share: the result's private part. The API is in
 * include/ghoti.io/text/html/ and is implemented in html_core.c (statuses, options, the result and
 * its error list), html_api.c (the parsers over the tree builder of html_tree_internal.h) and
 * html_file_io.c (the file form).
 */

#ifndef GHOTI_IO_GTEXT_SRC_HTML_HTML_API_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_HTML_HTML_API_INTERNAL_H

#include <ghoti.io/text/html.h>
#include <ghoti.io/text/macros.h>

#include "html_tree_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The private part of a GTEXT_HTML_Result: the kept parse errors, one allocation.
struct GTEXT_HTML_Result_Internal {
  GTEXT_Allocator allocator; ///< The one the block was made with, and is freed with.
  size_t count;              ///< Errors kept.
  size_t dropped;            ///< Errors raised and not kept.
  GTEXT_HTML_Error errors[]; ///< `count` of them.
};

/* The helpers below are shared by html_core.c, html_api.c and html_file_io.c and are not static, but they are not
   exported either: the library is built with -fvisibility=hidden and only GTEXT_API names are public (`nm -D` shows
   none of them, and make check-symbols holds the shared library to that), the same convention as xml_*, ht_* and markup_*. */

/// Zero a result (what every parse function does first).
void html_result_clear(GTEXT_HTML_Result * result);

/// Fill a result for a failure that has nothing to report but its status and message (NULL `message` is
/// the status's own).
void html_result_set_failure(
    GTEXT_HTML_Result * result, GTEXT_HTML_Status status, const char * message);

/**
 * @brief Copy the kept parse errors of a tree builder into a result.
 *
 * @return false if the allocator failed (the result then holds no errors).
 */
bool html_result_set_errors(GTEXT_HTML_Result * result, const GTEXT_Allocator * allocator,
    const GTEXT_HTML_Tree * tree);

/// Is this one of the Encoding standard's labels of UTF-8 (trimmed of ASCII white space, ASCII
/// case-insensitive)? A thin question over gtext_encoding_for_label(); kept for the tests that compare the
/// readers with the Encoding module.
GTEXT_INTERNAL_API bool html_label_is_utf8(const char * label);

/// Tests: make the parser read even UTF-8 through the Encoding module's decoder and the tokenizer's source map, the way it
/// reads every other encoding, instead of by the tokenizer's own reading of UTF-8. Must be called before the first byte is
/// fed (otherwise GTEXT_HTML_E_STATE, and nothing changes). The two must give the same tree, errors and locations; nothing else asks for this.
GTEXT_INTERNAL_API GTEXT_HTML_Status html_parser_force_transcode(struct GTEXT_HTML_Parser * parser);

/// Tests: the bytes the parser is holding for the sake of the encoding (the window it has not decided on, or the bytes it keeps for a
/// restart while the encoding is tentative and the head is not over): what "memory is bounded by the head" is measured by.
GTEXT_INTERNAL_API size_t html_parser_kept_bytes(const struct GTEXT_HTML_Parser * parser);

/// Check serializer options without making a serializer: OK, E_OPTIONS or E_UNSUPPORTED_ENCODING (NULL is the
/// defaults).
GTEXT_HTML_Status html_serialize_check_options(const GTEXT_HTML_Serialize_Options * opts);

/// The tree builder's status as the public one.
GTEXT_HTML_Status html_status_from_tree(GTEXT_HTML_TreeStatus status);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_HTML_HTML_API_INTERNAL_H
