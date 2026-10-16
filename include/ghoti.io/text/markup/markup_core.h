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
 * @file markup_core.h
 * @brief Status codes, options and the vocabulary of the markup tree.
 *
 * The tree is one type shared by XML, XHTML and HTML. The three have different
 * parsers - XML refuses what is not well formed, HTML never refuses anything -
 * and the same document model, so a consumer that walks inline SVG gets the
 * same shape whether the bytes arrived as a `.svg` file or inside a web page.
 * This header and markup_tree.h are that model; the parsers and writers for
 * each language are separate headers that produce and consume it.
 *
 * Nothing here reads or writes bytes of any language. See @ref
 * text_format_references for the formats built on it.
 */

#ifndef GHOTI_IO_GTEXT_MARKUP_MARKUP_CORE_H
#define GHOTI_IO_GTEXT_MARKUP_MARKUP_CORE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_Markup_Status
 * @brief What an operation on the tree, or on markup text, reports.
 *
 * Every function that can fail returns one of these and writes its result
 * through an output parameter **only on success**. A function that fails
 * leaves the tree exactly as it found it: there is no half-inserted node and
 * no partly renamed element, which tests/test-markup-model.cpp checks by
 * failing every allocation in turn and comparing the tree before and after.
 */
typedef enum {
  GTEXT_MARKUP_OK = 0,        ///< Success.
  GTEXT_MARKUP_E_INVALID,     ///< An argument was unusable: NULL where a
                              ///< pointer is required, an index out of range,
                              ///< a node of the wrong kind for the operation.
  GTEXT_MARKUP_E_OOM,         ///< Out of memory.
  GTEXT_MARKUP_E_LIMIT,       ///< A configured limit was exceeded: depth, node
                              ///< count or attributes per element.
  GTEXT_MARKUP_E_HIERARCHY,   ///< The insertion would break the tree's rules: a
                              ///< node inside itself, a text node directly
                              ///< under the document, a second root element, a
                              ///< doctype after the root element.
  GTEXT_MARKUP_E_WRONG_DOCUMENT, ///< A node belongs to another document. Use
                              ///< gtext_markup_clone() to bring a copy over.
  GTEXT_MARKUP_E_STATE,       ///< The operation is not valid in the node's
                              ///< current state, e.g. freeing a node that is
                              ///< still in the tree.
  GTEXT_MARKUP_E_NAME,        ///< A name that cannot be stored: empty, or
                              ///< containing a NUL.
  GTEXT_MARKUP_E_ENCODING,    ///< Bytes that are not well-formed UTF-8.
  GTEXT_MARKUP_E_UNREPRESENTABLE, ///< The text is valid and the language has
                              ///< no spelling for it: a control character XML
                              ///< 1.0 forbids even as a reference, `--` in a
                              ///< comment, `]]>` in a CDATA section.
  GTEXT_MARKUP_E_WRITE        ///< A sink refused output.
} GTEXT_Markup_Status;

/**
 * @brief A static, human-readable name for a status.
 *
 * @param status Any value; an unknown one returns "unknown status".
 * @return A string that is never freed.
 */
GTEXT_API const char * gtext_markup_status_string(GTEXT_Markup_Status status);

/**
 * @enum GTEXT_Markup_Kind
 * @brief What a node is.
 *
 * Zero is not a kind, so a zeroed value cannot be mistaken for one.
 */
typedef enum {
  GTEXT_MARKUP_KIND_NONE = 0,     ///< Not a node: what NULL reports.
  GTEXT_MARKUP_KIND_DOCUMENT,     ///< The root of a document. Exactly one per
                                  ///< document, created with it.
  GTEXT_MARKUP_KIND_FRAGMENT,     ///< A container with no parent of its own;
                                  ///< inserting one inserts its children.
  GTEXT_MARKUP_KIND_DOCTYPE,      ///< `<!DOCTYPE ...>`.
  GTEXT_MARKUP_KIND_ELEMENT,      ///< An element, with attributes.
  GTEXT_MARKUP_KIND_TEXT,         ///< Character data.
  GTEXT_MARKUP_KIND_CDATA,        ///< An XML CDATA section. HTML has none.
  GTEXT_MARKUP_KIND_COMMENT,      ///< `<!-- ... -->`.
  GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION, ///< `<?target data?>`.
  GTEXT_MARKUP_KIND_ENTITY_REFERENCE ///< An unresolved XML entity reference.
} GTEXT_Markup_Kind;

/**
 * @enum GTEXT_Markup_Mode
 * @brief The document mode HTML defines, which changes how a page is laid out.
 *
 * Carried here because the HTML parser decides it from the doctype and every
 * later consumer needs the answer; XML documents are always NO_QUIRKS.
 */
typedef enum {
  GTEXT_MARKUP_MODE_NO_QUIRKS = 0, ///< Standards mode. The default.
  GTEXT_MARKUP_MODE_LIMITED_QUIRKS, ///< "Almost standards" mode.
  GTEXT_MARKUP_MODE_QUIRKS          ///< Quirks mode.
} GTEXT_Markup_Mode;

/**
 * @enum GTEXT_Markup_Encoding_Source
 * @brief How a document's character encoding was decided.
 *
 * Recorded because a consumer that re-serializes needs to know whether the
 * label came from the bytes themselves or from somewhere that can be
 * overridden.
 */
typedef enum {
  GTEXT_MARKUP_ENCODING_UNKNOWN = 0, ///< Not recorded: a tree built by hand.
  GTEXT_MARKUP_ENCODING_BOM,         ///< A byte order mark.
  GTEXT_MARKUP_ENCODING_CALLER,      ///< The caller said so, as a transport
                                     ///< header would.
  GTEXT_MARKUP_ENCODING_DECLARATION, ///< The document's own declaration: an XML
                                     ///< declaration or an HTML `<meta>`.
  GTEXT_MARKUP_ENCODING_DEFAULT,     ///< Nothing said anything; the default.
  GTEXT_MARKUP_ENCODING_CALLER_DEFAULT ///< The caller's guess, not its word: the HTML parser's `default_encoding`,
                                     ///< or a transport label the caller said was only tentative.
} GTEXT_Markup_Encoding_Source;

/**
 * @enum GTEXT_Markup_Standalone
 * @brief The XML declaration's `standalone` pseudo-attribute.
 */
typedef enum {
  GTEXT_MARKUP_STANDALONE_UNSPECIFIED = 0, ///< Not present.
  GTEXT_MARKUP_STANDALONE_NO,              ///< `standalone="no"`.
  GTEXT_MARKUP_STANDALONE_YES              ///< `standalone="yes"`.
} GTEXT_Markup_Standalone;

/**
 * @struct GTEXT_Markup_Location
 * @brief Where in the source a node or attribute came from.
 *
 * `offset` counts decoded UTF-8 bytes in the physical input source, 0-based.
 * `line` and `column` are 1-based, and `column` counts characters. XML assigns
 * a nonzero `source_id` to each physical source; `reference_source_id` names
 * the source whose reference entered it. Zero is the no-source sentinel used
 * by hand-built and non-XML nodes. `raw_offset` is present only when a decoder
 * fault can be mapped to an original byte. `line` and `column` are held in 32
 * bits and saturate at UINT32_MAX; `offset` does not.
 */
typedef struct {
  size_t offset; ///< Byte offset into the input.
  size_t line;   ///< Line, 1-based.
  size_t column; ///< Column in characters, 1-based.
  /** Stable physical source number; zero means no XML source (hand-built or
   * non-XML node). The document source is 1; entity sources are assigned
   * monotonically for the parse. */
  uint32_t source_id;
  /** Source ID that referenced this source, or zero for the document source. */
  uint32_t reference_source_id;
  /** Original input byte offset when decoding changed the byte stream. */
  size_t raw_offset;
  /** Whether raw_offset is meaningful. */
  bool has_raw_offset;
} GTEXT_Markup_Location;

/** Kind of physical source recorded by an XML parse. */
typedef enum {
  GTEXT_MARKUP_SOURCE_DOCUMENT = 0,
  GTEXT_MARKUP_SOURCE_INTERNAL_ENTITY,
  GTEXT_MARKUP_SOURCE_EXTERNAL_ENTITY,
  GTEXT_MARKUP_SOURCE_PARAMETER_ENTITY,
  GTEXT_MARKUP_SOURCE_EXTERNAL_SUBSET
} GTEXT_Markup_Source_Kind;

/**
 * @struct GTEXT_Markup_Source
 * @brief Borrowed view of a document-owned physical source descriptor.
 *
 * Source IDs are local to one XML parse and remain stable for the lifetime of
 * its document. The document source has ID 1; zero is reserved for nodes with
 * no physical XML source. `reference_source_id` is zero for the document and
 * otherwise identifies the physical source that entered this source. The
 * descriptor's `name` is copied into the document and remains valid until the
 * document is freed. A clone imports the descriptors needed by cloned nodes.
 */
typedef struct {
  uint32_t id;
  uint32_t reference_source_id;
  GTEXT_Markup_Source_Kind kind;
  const char * name; ///< Entity name or resolved base URI; NULL if unknown.
} GTEXT_Markup_Source;

/**
 * @struct GTEXT_Markup_Text_Span
 * @brief One contiguous run in a text node's mixed-source provenance map.
 *
 * `text_offset` and `length` are byte offsets into the node's UTF-8 text.
 * The associated location identifies the physical source and its decoded
 * offset at the start of the run. Adjacent source runs are retained when XML
 * DTD processing coalesces replacement text into one DOM text node. A span
 * array belongs to its node and is copied by clone; its descriptor IDs resolve
 * through the owning document's source table.
 */
typedef struct {
  size_t text_offset;
  size_t length;
  GTEXT_Markup_Location location;
} GTEXT_Markup_Text_Span;

/**
 * @struct GTEXT_Markup_Options
 * @brief What a document is allowed to do.
 *
 * Zeroed is not the default: use gtext_markup_options_default() and change
 * what you mean to change. A zeroed struct asks for no limit on anything.
 */
typedef struct {
  /**
   * The allocator every allocation of the document goes through, or NULL for
   * gtext_allocator_default(). The struct is copied; the `ctx` it holds must
   * outlive the document.
   */
  const GTEXT_Allocator * allocator;

  /**
   * Deepest nesting an insertion may produce, counting the document as depth
   * 0, or 0 for no limit. Default 256.
   *
   * An insertion that would put any node deeper than this fails with
   * GTEXT_MARKUP_E_LIMIT, **including the descendants of a subtree being
   * moved**. The check walks the inserted subtree when it has children, so
   * moving a large subtree costs time proportional to its size; the parsers
   * track their own depth and do not pay it.
   */
  size_t max_depth;

  /**
   * Most nodes the document may hold at once, detached ones and template
   * contents included, or 0 for no limit. Default 0.
   */
  size_t max_nodes;

  /**
   * Most attributes one element may hold, or 0 for no limit. Default 1024.
   * An attribute lookup or insertion scans the element's attributes, so a
   * bound keeps those linear in something small.
   */
  size_t max_attributes;
} GTEXT_Markup_Options;

/**
 * @brief The defaults: allocator NULL, max_depth 256, no node limit, 1024
 *   attributes per element.
 *
 * @return The defaults, by value.
 */
GTEXT_API GTEXT_Markup_Options gtext_markup_options_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_MARKUP_MARKUP_CORE_H
