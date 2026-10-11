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
 * @file xml_core.h
 * @brief Status codes, options and errors of the XML parser.
 *
 * The parser reads UTF-8 XML 1.0 (Fifth Edition) with Namespaces in XML 1.0
 * and builds a @ref GTEXT_Markup_Document; the tree itself is described in
 * markup_tree.h. Well-formedness is the contract: a document that violates it
 * yields an error with a position and **no tree**.
 *
 * Mirrors csv_core.h: an options struct with a `_default()`, a status enum, an
 * error struct that carries the position, and `_error_free()`.
 */

#ifndef GHOTI_IO_GTEXT_XML_XML_CORE_H
#define GHOTI_IO_GTEXT_XML_XML_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_XML_Status
 * @brief What parsing XML reports.
 *
 * The first block is not about the document; the second is "not well-formed",
 * split by what was wrong so a caller can act on the difference; the third is
 * the limits, each its own status, because a document that is refused for being
 * too deep is not a document that is malformed.
 */
typedef enum {
  GTEXT_XML_OK = 0,   ///< Success.
  GTEXT_XML_E_INVALID, ///< An argument was unusable (NULL output, NULL data
                       ///< with a length).
  GTEXT_XML_E_OOM,     ///< Out of memory. No tree, and nothing leaked.

  GTEXT_XML_E_SYNTAX,  ///< Not well-formed in a way no other status names: a
                       ///< production did not match.
  GTEXT_XML_E_UNEXPECTED_EOF, ///< The input ended inside a construct, or
                       ///< before the root element.
  GTEXT_XML_E_UTF8,    ///< Bytes that are not well-formed UTF-8.
  GTEXT_XML_E_CHARACTER, ///< Well-formed UTF-8 that is not an XML 1.0
                       ///< character (a C0 control other than tab, line feed
                       ///< and carriage return, U+FFFE, U+FFFF), or a numeric
                       ///< character reference to one.
  GTEXT_XML_E_MISMATCHED_TAG, ///< An end tag that does not match its start
                       ///< tag. The position is the end tag's.
  GTEXT_XML_E_DUPLICATE_ATTRIBUTE, ///< The same attribute name twice on one
                       ///< element, written alike or expanding to the same
                       ///< namespace and local name. The position is the
                       ///< second one's.
  GTEXT_XML_E_ENTITY,  ///< An entity reference other than the five predefined
                       ///< ones. The position is the `&`.
  GTEXT_XML_E_NAMESPACE, ///< Well-formed XML that breaks Namespaces in XML: an
                       ///< unbound prefix, a name with two colons, a misuse of
                       ///< the reserved `xml` and `xmlns` prefixes.
  GTEXT_XML_E_DOCTYPE, ///< A malformed or unterminated document type
                       ///< declaration.
  GTEXT_XML_E_UNSUPPORTED_ENCODING, ///< EBCDIC (by its first bytes), or
                       ///< an XML declaration whose `encoding` is none of
                       ///< UTF-8, UTF-16, UTF-32, ISO-8859-1 and US-ASCII.
                       ///< Not a verdict on the document: it may be
                       ///< well-formed.
  GTEXT_XML_E_UNSUPPORTED_VERSION, ///< An XML declaration with a version other
                       ///< than 1.0. Not a verdict on the document.

  GTEXT_XML_E_LIMIT_INPUT,      ///< The input is longer than `max_input_bytes`.
  GTEXT_XML_E_LIMIT_DEPTH,      ///< Elements nested deeper than `max_depth`.
  GTEXT_XML_E_LIMIT_ATTRIBUTES, ///< More than `max_attributes` on an element.
  GTEXT_XML_E_LIMIT_VALUE,      ///< An attribute value longer than
                                ///< `max_attribute_bytes`.
  GTEXT_XML_E_LIMIT_NAME,       ///< A name longer than `max_name_bytes`.
  GTEXT_XML_E_LIMIT_NODES,      ///< More than `max_nodes` nodes in the tree.
  GTEXT_XML_E_LIMIT_NAMESPACES, ///< More than `max_namespaces` namespace
                                ///< declarations in scope at once.

  GTEXT_XML_E_INVALID_SEQUENCE, ///< Bytes that are not valid in the document's
                       ///< encoding: a lone or reversed UTF-16 surrogate, a
                       ///< UTF-32 unit past U+10FFFF or in the surrogate
                       ///< range, a document that ends inside a code unit, a
                       ///< byte above 0x7F under `US-ASCII`. The position is
                       ///< the offending unit's.
  GTEXT_XML_E_ENCODING_MISMATCH, ///< The declaration's `encoding` names a
                       ///< different family from the one the document is in
                       ///< (a UTF-16 document labeled UTF-8, a UTF-8 document
                       ///< labeled UTF-16). The position is the label's.

  GTEXT_XML_E_INCOMPLETE, ///< Event parsers only: the pull reader has no event
                       ///< to give until it is fed more input. Not an error
                       ///< in the document.
  GTEXT_XML_E_STATE    ///< Event parsers only: the call does not fit the
                       ///< parser's state - input after the end of input, or
                       ///< a pull reader that has delivered its last event.
} GTEXT_XML_Status;

/**
 * @brief A static, human-readable name for a status.
 *
 * @param status Any value; an unknown one returns "unknown status".
 * @return A string that is never freed.
 */
GTEXT_API const char * gtext_xml_status_string(GTEXT_XML_Status status);

/**
 * @struct GTEXT_XML_Error
 * @brief Why a parse failed, and where.
 *
 * `byte_offset` is 0-based into the bytes the parser was given, a byte order
 * mark included. `line` and `column` are 1-based, and `column` counts
 * characters. A line ends at LF, CR or CRLF, which is XML's own rule.
 * The position is that of the construct at fault: the `&` of a bad reference,
 * the `</` of a mismatched end tag, the second of two equal attributes, the
 * token that went over a limit; for anything else, the first character the
 * parser could not accept.
 */
typedef struct {
  GTEXT_XML_Status code; ///< The status; GTEXT_XML_OK if nothing failed.
  const char * message;  ///< A static, human-readable description. Never freed.
  size_t byte_offset;    ///< Byte offset into the input (0-based).
  size_t line;           ///< Line (1-based).
  size_t column;         ///< Column, in characters (1-based).
} GTEXT_XML_Error;

/**
 * @struct GTEXT_XML_Parse_Options
 * @brief What a parse may do.
 *
 * Every limit has a default from the first release: a field left at 0 selects
 * it, and 0 never means "unlimited" (SIZE_MAX is as close as the type allows).
 * Zeroed is therefore the same as gtext_xml_parse_options_default().
 */
typedef struct {
  /**
   * The allocator every allocation of the parse and of the resulting document
   * goes through, or NULL for gtext_allocator_default(). The struct is copied;
   * the `ctx` it holds must outlive the document.
   */
  const GTEXT_Allocator * allocator;

  /** Longest input, in bytes. Default 64 MiB. */
  size_t max_input_bytes;

  /**
   * Deepest element nesting, the root element being depth 1. Default 256.
   * The parser keeps its own stack on the heap, so this bounds memory and the
   * tree's later operations, not the C stack.
   */
  size_t max_depth;

  /** Most attributes on one element, namespace declarations included.
   *  Default 1024. */
  size_t max_attributes;

  /** Longest attribute value after normalization, in bytes. Default 16 MiB. */
  size_t max_attribute_bytes;

  /** Longest name (element, attribute, processing instruction target,
   *  document type), in bytes. Default 8192 (the conformance suite has a
   *  processing instruction target of 4,248). */
  size_t max_name_bytes;

  /** Most nodes in the document, the document node included. Default
   *  10,000,000. */
  size_t max_nodes;

  /** Most namespace declarations in scope at one element, counting a nested
   *  redeclaration of a prefix separately. Default 256. */
  size_t max_namespaces;
} GTEXT_XML_Parse_Options;

/**
 * @brief The defaults, spelled out.
 *
 * @return The defaults, by value: allocator NULL, 64 MiB of input, depth 256,
 *   1024 attributes, 16 MiB of attribute value, 8192 bytes of name, ten million
 *   nodes, 256 namespace declarations.
 */
GTEXT_API GTEXT_XML_Parse_Options gtext_xml_parse_options_default(void);

/**
 * @brief Release what an error holds.
 *
 * Nothing today: the message is static. It exists so that callers write the
 * same cleanup now that they will need if an error ever owns memory, and it
 * resets the struct so a stale error cannot be read again. Safe on NULL and on
 * an error that was never filled.
 *
 * @param err The error to reset.
 */
GTEXT_API void gtext_xml_error_free(GTEXT_XML_Error * err);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_XML_XML_CORE_H
