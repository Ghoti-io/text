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
 * The shape of the embedded meta-schemas.
 *
 * Hand-written; only metaschema/metaschema_docs.c is generated.
 */

#ifndef GHOTI_IO_GTEXT_SRC_JSON_METASCHEMA_METASCHEMA_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_JSON_METASCHEMA_METASCHEMA_INTERNAL_H

#include <ghoti.io/text/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One published document, under the URI that names it
 *
 * The bytes are exactly as published, held one source line at a time and
 * joined when the document is used. A whole document in one string literal
 * would be the obvious shape and is not available: C99 guarantees only 4,095
 * characters in a string literal after concatenation (5.2.4.1), and the
 * draft-07 meta-schema is 4,979 bytes. Every compiler this library is built
 * with accepts more, so the alternative was a pedantic diagnostic suppressed
 * in one file - but the limit is real, it is somebody else's document that
 * decides whether it is met, and the next document embedded here could be
 * larger again.
 *
 * There is deliberately no stored length. The document's size is the sum of
 * its lines' lengths, computed where it is joined, so there is no second
 * number that can disagree with the bytes. The generator refuses a document
 * containing a NUL for the same reason: that is what makes the sum exact.
 */
typedef struct {
  const char * uri;             ///< Absolute, no fragment
  const char * const * lines;   ///< The document's lines, terminators included
  size_t line_count;
} json_metaschema_doc;

/**
 * Every published meta-schema of every dialect this library reads: nineteen
 * documents.
 *
 * 2020-12 has nine - the root meta-schema, the seven vocabulary meta-schemas
 * its `allOf` references, and format-assertion, which describes the dialect's
 * one optional vocabulary and so is referenced by schemas that declare it
 * rather than by the root. 2019-09 has seven, having one `format` vocabulary
 * rather than two and keeping the `unevaluated*` keywords in `applicator`.
 *
 * draft-07, draft-06 and draft-04 have one each, because `$vocabulary` arrived
 * in 2019-09 and before it the keyword set is the draft rather than a
 * declaration inside it. Their `uri` here is the fragmentless form, which is
 * what a reference resolves to before its fragment is read - a document in the
 * wild refers to them as `http://json-schema.org/draft-07/schema#`, and that is
 * the same identity.
 */
extern const json_metaschema_doc json_metaschema_docs[];
extern const size_t json_metaschema_doc_count;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_JSON_METASCHEMA_METASCHEMA_INTERNAL_H
