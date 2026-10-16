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
 * @brief Status codes, options and errors of the XML parser and writer.
 *
 * The parser reads XML 1.0 (Fifth Edition) or XML 1.1 (Second Edition), selected
 * by the XML declaration (XML 1.0 when absent), and builds a
 * @ref GTEXT_Markup_Document; the tree itself is described in
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
#include <ghoti.io/text/markup/markup_core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
  GTEXT_XML_E_CHARACTER, ///< A character forbidden by the selected XML
                       ///< version, or a numeric character reference to one.
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
                       ///< than 1.0 or 1.1. Not a verdict on the document.

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
  GTEXT_XML_E_STATE,   ///< Event parsers only: the call does not fit the
                       ///< parser's state - input after the end of input, or
                       ///< a pull reader that has delivered its last event.

  GTEXT_XML_E_WRITE,   ///< Writer only: a sink (or the file being written)
                       ///< refused output.
  GTEXT_XML_E_UNREPRESENTABLE, ///< Writer only: the tree holds something XML
                       ///< has no spelling for: `--` in or a trailing `-` on a
                       ///< comment, `?>` or leading white space in processing
                       ///< instruction data, `]]>` in a CDATA section, a
                       ///< carriage return in a comment, processing
                       ///< instruction or CDATA section; a document with no
                       ///< root element or a second one; template content; a
                       ///< node of a kind with no place where it is; or a
                       ///< character the output encoding has no byte for in a
                       ///< place with no character reference (a name, comment,
                       ///< processing instruction, CDATA section or
                       ///< identifier).
  GTEXT_XML_E_NAME,    ///< Writer only: a name that is not an XML name: an
                       ///< element, attribute or prefix that is not an NCName,
                       ///< a processing instruction target that is not an
                       ///< NCName or is `xml` in any case, or a document type
                       ///< name that is not a Name.
  GTEXT_XML_E_LIMIT_OUTPUT, ///< Writer only: the output would be longer than
                       ///< `max_output_bytes`.
  GTEXT_XML_E_XHTML,   ///< Writer only: `xhtml` mode refuses a tree that is
                       ///< XML but that an HTML parser would read as a
                       ///< different tree (a void element with children,
                       ///< `<` in a script, a processing instruction, ...), or,
                       ///< with `xhtml_strict`, one with a tree-construction
                       ///< hazard. The result's message and path say which.
  GTEXT_XML_E_UNSUPPORTED_MODE, ///< Reserved for an unavailable optional mode.
  /* New statuses are appended to keep the values above source and ABI stable. */
  GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE, ///< One external source exceeded its byte budget.
  GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL, ///< Aggregate external bytes exceeded their budget.
  GTEXT_XML_E_LIMIT_EXPANSION, ///< Cumulative entity replacement bytes exceeded their budget.
  GTEXT_XML_E_LIMIT_ENTITY_DEPTH, ///< Active entity nesting exceeded its budget.
  GTEXT_XML_E_LIMIT_REFERENCES, ///< XML references exceeded their budget.
  GTEXT_XML_E_LIMIT_DECLARATIONS, ///< DTD declarations exceeded their budget.
  GTEXT_XML_E_LIMIT_WORK, ///< Parser work exceeded its deterministic budget.
  GTEXT_XML_E_RESOLVER, ///< The resolver returned FAILED, OPENED metadata
                        ///< was malformed, or a source read returned an error
                        ///< or invalid byte count. A declined external subset
                        ///< or parameter entity continues with incomplete DTD
                        ///< knowledge instead of returning this status.
  GTEXT_XML_E_CANCELLED, ///< The caller cancelled an external source read.
  GTEXT_XML_E_UNAVAILABLE_RESOURCE ///< A referenced external entity was unavailable.
} GTEXT_XML_Status;

/** A bounded DTD validity finding produced by DTD_VALIDATE mode. */
typedef enum {
  GTEXT_XML_VALIDATION_ROOT_TYPE,
  GTEXT_XML_VALIDATION_UNDECLARED_ELEMENT,
  GTEXT_XML_VALIDATION_ELEMENT_CONTENT,
  GTEXT_XML_VALIDATION_NONDETERMINISTIC_MODEL,
  GTEXT_XML_VALIDATION_DUPLICATE_ELEMENT_DECLARATION,
  GTEXT_XML_VALIDATION_ATTRIBUTE_VALUE,
  GTEXT_XML_VALIDATION_DUPLICATE_ID,
  GTEXT_XML_VALIDATION_UNRESOLVED_IDREF,
  GTEXT_XML_VALIDATION_DTD_CROSS_REFERENCE,
  GTEXT_XML_VALIDATION_STANDALONE
} GTEXT_XML_Validation_Code;

/** One DTD validity finding. `message` is library-owned static text. */
typedef struct {
  GTEXT_XML_Validation_Code code;
  GTEXT_Markup_Location location;
  const char * message;
} GTEXT_XML_Validation_Diagnostic;

/** Final DTD validation state for a successfully completed VALIDATE parse. */
typedef enum {
  GTEXT_XML_VALIDATION_NOT_RUN = 0,
  GTEXT_XML_VALIDATION_VALID,
  GTEXT_XML_VALIDATION_INVALID,
  GTEXT_XML_VALIDATION_INCOMPLETE
} GTEXT_XML_Validation_Verdict;

/** Summary kept separate from the XML operation status. */
typedef struct {
  GTEXT_XML_Validation_Verdict verdict;
  bool knowledge_complete;
  bool diagnostics_truncated;
  size_t diagnostic_count;
} GTEXT_XML_Validation_Result;

/** DTD behavior selected for a parse. Zero preserves the historical default. */
typedef enum {
  GTEXT_XML_DTD_OFF = 0,
  GTEXT_XML_DTD_PROCESS = 1,
  GTEXT_XML_DTD_VALIDATE = 2
} GTEXT_XML_DTD_Mode;

/**
 * Policy for unavailable external general entities. External access is never
 * implicit; SKIP is valid only with non-validating PROCESS mode. A declined
 * referenced external entity is retained as an unresolved placeholder and
 * reported as a skipped-entity event, not replaced with empty text.
 */
typedef enum {
  GTEXT_XML_DTD_EXTERNAL_DENY = 0,
  GTEXT_XML_DTD_EXTERNAL_SKIP = 1
} GTEXT_XML_DTD_External_Policy;

/** External resource kind requested from a caller resolver. */
typedef enum {
  GTEXT_XML_RESOURCE_EXTERNAL_SUBSET = 0,
  GTEXT_XML_RESOURCE_GENERAL_ENTITY,
  GTEXT_XML_RESOURCE_PARAMETER_ENTITY
} GTEXT_XML_Resource_Kind;

/** Explicit per-parse permission to ask the caller resolver for external data. */
typedef enum {
  GTEXT_XML_EXTERNAL_ACCESS_DENY = 0,
  GTEXT_XML_EXTERNAL_ACCESS_ALLOW = 1
} GTEXT_XML_External_Access;

/** Resolver request metadata. Identifier strings are opaque and borrowed for
 *  the duration of the resolver callback. base_uri is the active declaration
 *  source's base URI. The bounded reference_trail contains labels for active
 *  physical sources from outermost retained parent to innermost source; it is
 *  empty for a top-level external subset. */
typedef struct {
  GTEXT_XML_Resource_Kind kind;
  const char * public_id;
  const char * system_id;
  const char * base_uri;
  const char * const * reference_trail;
  size_t reference_trail_count;
} GTEXT_XML_Resolve_Request;

/** Outcome of reading a caller-owned synchronous source. */
typedef enum {
  GTEXT_XML_SOURCE_DATA = 0,
  GTEXT_XML_SOURCE_EOF,
  GTEXT_XML_SOURCE_ERROR,
  GTEXT_XML_SOURCE_CANCELLED
} GTEXT_XML_Source_Read_Result;

/** Source callbacks are synchronous. DATA must return between 1 and `capacity`
 *  bytes; EOF must return zero bytes. Other result/count combinations fail
 *  with GTEXT_XML_E_RESOLVER. The parser copies each returned chunk before the
 *  next read and calls close exactly once after a successful open.
 *  `identity`, `base_uri`, `read`, and `close` must be non-NULL; identity and
 *  base_uri stay valid until close returns. If other OPENED metadata is invalid
 *  but `close` is present, the parser calls it once before returning
 *  GTEXT_XML_E_RESOLVER. Without `close`, the resolver handle cannot be
 *  released. Identity is compared by pointer
 *  value against active sources: return the same identity token for the same
 *  physical resource, including aliases, and distinct tokens for distinct
 *  resources. The parser copies base_uri for later nested-resource resolution. */
typedef struct {
  void * handle;
  const void * identity;
  const char * base_uri;
  GTEXT_XML_Source_Read_Result (*read)(void * handle, unsigned char * buffer,
      size_t capacity, size_t * bytes_read);
  void (*close)(void * handle);
} GTEXT_XML_External_Source;

/** Outcome of asking the resolver to open a resource. */
typedef enum {
  GTEXT_XML_RESOLVER_OPENED = 0,
  GTEXT_XML_RESOLVER_DECLINED,
  GTEXT_XML_RESOLVER_FAILED
} GTEXT_XML_Resolver_Result;

/** Caller callback. Return OPENED only after filling every source field,
 *  including a non-NULL close callback; OPENED transfers the exactly-once
 *  close obligation to the parser. Malformed OPENED metadata fails immediately
 *  with GTEXT_XML_E_RESOLVER and is not retried. Return DECLINED when
 *  unavailable, or FAILED for an operation failure. DECLINED for an external
 *  parameter entity or subset records incomplete DTD knowledge and continues
 *  non-validating processing. A declined external general entity produces
 *  GTEXT_XML_E_UNAVAILABLE_RESOURCE by default, or is skipped only under the
 *  explicit SKIP policy. FAILED for any resource kind produces
 *  GTEXT_XML_E_RESOLVER. A source read error also produces
 *  GTEXT_XML_E_RESOLVER; SOURCE_CANCELLED produces GTEXT_XML_E_CANCELLED. */
typedef GTEXT_XML_Resolver_Result (*GTEXT_XML_Resolver_cb)(
    const GTEXT_XML_Resolve_Request * request, void * user_data,
    GTEXT_XML_External_Source * out_source);

/**
 * @brief A static, human-readable name for a status.
 *
 * @param status Any value; an unknown one returns "unknown status".
 * @return A string that is never freed. GTEXT_XML_E_UNSUPPORTED_MODE names an
 *   XML mode the library does not implement.
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
 *
 * `source_id` and `reference_source_id` identify the physical source and its
 * immediate reference parent. `raw_byte_offset` is set for decoding faults
 * when available; `byte_offset` remains the decoded coordinate for parser
 * errors.
 */
typedef struct {
  GTEXT_XML_Status code; ///< The status; GTEXT_XML_OK if nothing failed.
  const char * message;  ///< A static, human-readable description. Never freed.
  size_t byte_offset;    ///< Byte offset into the input (0-based).
  size_t line;           ///< Line (1-based).
  size_t column;         ///< Column, in characters (1-based).
  uint32_t source_id;    ///< Physical source number, zero if unavailable.
  uint32_t reference_source_id; ///< Source that referenced source_id, if any.
  size_t raw_byte_offset; ///< Original-byte coordinate for decoding faults.
  bool has_raw_byte_offset;
  uint8_t reference_trail_count;
  bool reference_trail_truncated;
  uint32_t reference_trail[32]; ///< Source IDs from outermost retained parent to fault source.
} GTEXT_XML_Error;

/**
 * @struct GTEXT_XML_Parse_Options
 * @brief What a parse may do.
 *
 * Every limit has a default from the first release: a field left at 0 selects
 * it, and 0 never means "unlimited" (SIZE_MAX is as close as the type allows).
 * Zeroed is therefore the same as gtext_xml_parse_options_default(). DTD mode
 * and external-entity policy are also zero-defaulted: DTD_OFF and DENY.
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

  /** Maximum bytes read from one external source. Default 16 MiB. */
  size_t max_external_source_bytes;
  /** Maximum bytes read across all external sources. Default 64 MiB. */
  size_t max_external_bytes;
  /** Maximum replacement bytes processed cumulatively. Default 64 MiB. */
  size_t max_expanded_bytes;
  /** Maximum active entity sources, excluding the document. Default 32. */
  size_t max_entity_depth;
  /** Maximum XML references processed, including entity and character references. Default 1,000,000. */
  size_t max_references;
  /** Maximum DTD declarations processed. Default 100,000. */
  size_t max_declarations;
  /** Maximum charged parser work units. Default 256 MiB. */
  size_t max_total_work;

  /** Maximum DTD content-model compilation and matching work units. Default 64 Mi. */
  size_t max_validation_work;
  /** Maximum temporary matcher and determinism-analysis state. Default 16 MiB. */
  size_t max_validation_memory;
  /** Maximum retained/emitted structural validity findings. Default 256. */
  size_t max_validation_diagnostics;

  /** DTD mode; zero selects GTEXT_XML_DTD_OFF. */
  GTEXT_XML_DTD_Mode dtd_mode;

  /** External entity policy; zero selects GTEXT_XML_DTD_EXTERNAL_DENY. */
  GTEXT_XML_DTD_External_Policy dtd_external_policy;

  /** External resources are never requested unless this is ALLOW. */
  GTEXT_XML_External_Access external_access;
  /** Optional resolver; used with PROCESS or VALIDATE mode and
   *  external_access ALLOW.
   *  A resolver may perform I/O, so applications must enforce their own
   *  allowlists and transport limits. */
  GTEXT_XML_Resolver_cb resolver;
  void * resolver_user_data;
  /** Base URI of the document source, or NULL when unknown. The string must
   *  remain valid until a streaming parser is freed; whole-input parsing uses
   *  it only for the duration of the call. */
  const char * base_uri;

  /** Byte threshold for queued event copies in a pull reader. Default 1 MiB.
   *  This counts undelivered events; the most recently returned event remains
   *  valid until the next reader_next() call and is outside the queue threshold.
   *  A parser step may cross the threshold before pausing; DTD parameter
   *  references yield individually so a long subset cannot fill the queue in
   *  one step. */
  size_t max_pull_queue_bytes;
  /** Event threshold for the pull queue. Default 256. Parsing pauses after
   *  completing the step that reaches it and does not resume until drained
   *  below both limits. */
  size_t max_pull_queue_events;
} GTEXT_XML_Parse_Options;

/**
 * @brief The defaults, spelled out.
 *
 * @return The defaults, by value: allocator NULL, 64 MiB of input, depth 256,
 *   1024 attributes, 16 MiB of attribute value, 8192 bytes of name, ten million
 *   nodes, 256 namespace declarations, 16 MiB per external source, 64 MiB
 *   aggregate external bytes and expanded bytes, entity depth 32, one million
 *   XML references, 100,000 declarations, 256 MiB total work, DTD_OFF mode, and
 *   external DENY policy, 1 MiB / 256 queued pull events. All zero-valued
 *   limit overrides select these finite defaults.
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
