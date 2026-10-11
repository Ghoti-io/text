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
 * @file xml_parse.h
 * @brief Parse XML into a markup tree.
 */

#ifndef GHOTI_IO_GTEXT_XML_XML_PARSE_H
#define GHOTI_IO_GTEXT_XML_XML_PARSE_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_tree.h>
#include <ghoti.io/text/xml/xml_core.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a whole XML document into a tree.
 *
 * The input is UTF-8 XML 1.0 with Namespaces in XML 1.0. What is built:
 *
 * - **Elements** with their namespace URI, prefix and local name; a default
 *   namespace applies to elements and not to attributes. Namespace
 *   declarations stay in the tree as attributes in the `xmlns` namespace
 *   (`xmlns="u"` is prefix NULL, local `xmlns`; `xmlns:p="u"` is prefix
 *   `xmlns`, local `p`), in source order with the others.
 * - **Text**, with references to the five predefined entities and to
 *   characters already expanded, and CR LF and lone CR already turned into LF.
 *   Adjacent character data is one node; a CDATA section is a node of its own,
 *   a comment and a processing instruction likewise. Whitespace outside the
 *   root element is not kept.
 * - **Attribute values** normalized as for CDATA: each tab, line feed and
 *   carriage return, written literally, becomes a space; a character
 *   reference to one stays itself.
 * - **The document type** as a doctype node (name, public and system
 *   identifier). Its internal subset is **skipped, not processed**: a bounded
 *   scanner finds its true end, so a `]` inside a quoted literal or a comment
 *   does not end it, and an unterminated subset is an error. No declaration in
 *   it takes effect - not an entity, not a default attribute.
 * - **The XML declaration** as the document's version, standalone flag and
 *   encoding label (see gtext_markup_document_xml_version() and
 *   gtext_markup_document_encoding()). With no declaration the version is NULL
 *   and the encoding is "UTF-8" from the default.
 * - **Source locations** on every element, text, comment, processing
 *   instruction and doctype node, and on every attribute.
 *
 * Nothing is read from a file or a network: a reference to any entity but the
 * five predefined ones is an error (GTEXT_XML_E_ENTITY), whether or not the
 * document type declares it, and an external identifier is recorded and never
 * opened.
 *
 * **Encodings.** UTF-8 only. A UTF-8 byte order mark is accepted and skipped.
 * UTF-16 and UTF-32 by their byte order mark or by the first bytes, EBCDIC by
 * its first bytes, and an XML declaration whose `encoding` is anything but
 * `UTF-8` (case-insensitive) return GTEXT_XML_E_UNSUPPORTED_ENCODING, which
 * says nothing about whether the document is well-formed. An XML declaration
 * with a version other than 1.0 returns GTEXT_XML_E_UNSUPPORTED_VERSION.
 *
 * @param data   The document. Need not be NUL-terminated and is not kept; may
 *               be NULL only if `len` is 0 (which is an empty document, and
 *               not well-formed).
 * @param len    Bytes of `data`.
 * @param opts   Options, or NULL for gtext_xml_parse_options_default().
 * @param out    Receives the document, which the caller frees with
 *               gtext_markup_document_free(). Written **only on success**.
 * @param err    Receives the failure, or a cleared error on success; may be
 *               NULL.
 * @return GTEXT_XML_OK, or why the parse failed. On any failure there is no
 *   tree and nothing is leaked, including when the allocator fails at any
 *   point.
 */
GTEXT_API GTEXT_XML_Status gtext_xml_parse(const void * data, size_t len,
    const GTEXT_XML_Parse_Options * opts, GTEXT_Markup_Document ** out,
    GTEXT_XML_Error * err);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_XML_XML_PARSE_H
