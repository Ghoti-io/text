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
 * @file xml_writer.h
 * @brief Write a markup document as XML 1.0 or XML 1.1, to a sink, a buffer or a file.
 *
 * The document's recorded version selects XML 1.0 (Fifth Edition) or XML 1.1
 * (Second Edition) and the matching Namespaces Recommendation. No declaration
 * selects XML 1.0. Unsupported recorded versions are refused even when the
 * output declaration is omitted.
 *
 * ## The contract
 *
 * For documents without a doctype, `gtext_xml_parse(write(tree))` gives
 * back a tree equal to it (gtext_markup_equal(); locations are not compared),
 * and writing that tree again gives the same bytes. For DTD-aware round trips,
 * the reader must use matching DTD processing and identifier policy, and any
 * external resources must be equivalent to those supplied for the original
 * parse. Default omission of an external identifier is reported as reduced
 * output and does not claim full DTD-semantic equivalence. The two exceptions
 * are the ones XML cannot tell apart, and they are reported rather than hidden
 * (GTEXT_XML_Write_Result::text_nodes_coalesced): an empty text node, and a text
 * node next to another text node, read back as nothing and as one node. Pretty
 * printing (GTEXT_XML_Write_Options::indent) adds white space and so changes the
 * tree by design; the result says how many elements it did that to and how many
 * it left alone.
 *
 * A tree XML has no spelling for is **refused, not repaired**, with a status
 * and the path of the node at fault:
 *
 * - a comment that contains `--` or ends in `-`, processing instruction data
 *   that contains `?>`, a CDATA section that contains `]]>`;
 * - a carriage return in a comment, processing instruction or CDATA section
 *   (XML reads every carriage return as a line feed, and these have no
 *   reference to hide it behind), and processing instruction data that starts
 *   with white space (the parser takes it as the separator);
 * - a character forbidden by the selected XML version (GTEXT_XML_E_CHARACTER);
 *   in XML 1.1, restricted characters, NEL and LINE SEPARATOR in text and
 *   attribute values are written as character references to preserve their
 *   values; these characters are refused in names and raw syntax such as
 *   comments, processing instructions, CDATA and document type identifiers;
 * - a name that is not an XML name, or a processing instruction target `xml`
 *   in any case (GTEXT_XML_E_NAME);
 * - an element or attribute whose prefix has no in-scope declaration for its
 *   namespace, an `xmlns` attribute that contradicts the namespace of the
 *   element that carries it, the reserved `xml` and `xmlns` prefixes used for
 *   anything else, an attribute in a namespace with no prefix, a prefix with
 *   no namespace (GTEXT_XML_E_NAMESPACE);
 * - a character the output encoding has no byte for, in a place with no
 *   reference to hide it behind: a name, a comment, a processing instruction,
 *   a CDATA section, an identifier in the document type (GTEXT_XML_E_UNREPRESENTABLE);
 * - a document type that cannot be spelled (GTEXT_XML_E_DOCTYPE), a template
 *   element's content (it is not a child), a document with no root element.
 *
 * **A refusal writes nothing to the sink.** The whole document is checked first
 * (the same walk, with no output), and bytes go out only when it has passed;
 * the output-size limit is part of that check. A sink that fails during the
 * second walk, or an allocator that does, stops it with GTEXT_XML_E_WRITE or
 * GTEXT_XML_E_OOM and the sink has then received a prefix of the document.
 *
 * The document must not be modified, from another thread or from the sink, while
 * a write is running; the writer reads it twice.
 *
 * ## Choices
 *
 * - Attributes are written in tree order, in double quotes. A text or attribute
 *   value is escaped for its place: `&`, `<`, `>`, `"` (attributes only), a
 *   carriage return as `&#13;`, and a tab or line feed in an attribute value as
 *   `&#9;` and `&#10;`, which is what keeps them from being read back as spaces.
 * - XML 1.1 prefix undeclarations are written when present; XML 1.0 prefix
 *   undeclarations remain a namespace error.
 * - Under ISO-8859-1 and US-ASCII a character the encoding lacks is written as
 *   a hexadecimal character reference in text and attribute values. XML 1.1
 *   restricted characters are also written as references. Both are counted in
 *   GTEXT_XML_Write_Result::references_written.
 * - Pretty printing indents only inside an element whose children are all
 *   elements, comments and processing instructions, and never in or below an
 *   element with `xml:space="preserve"` (an `xml:space="default"` below it does
 *   not undo that: the safe reading). An element with a text or CDATA child, and
 *   everything under it, is left exactly as it is. The newline option is used
 *   between the XML declaration, the document type, the other top-level items and
 *   the root element, whether or not indentation is on. Because the result is
 *   read back as extra text nodes, `write(parse(write(t)))` is the same bytes
 *   for a pretty-printed tree only when the newline is a bare line feed.
 * - A document type is written from its name, identifiers, and retained raw
 *   internal syntax. The declarations, parameter-entity references, comments,
 *   and processing instructions in that subset are emitted in retained order.
 *   External snapshots and declarations generated or indexed from them are
 *   not copied into the internal subset. The syntax is validated against the
 *   document's XML version and converted through the selected output encoding.
 *   `internal_subset_emitted` reports whether retained internal syntax was
 *   written. `doctype_without_subset` is its inverse for a written doctype;
 *   both are false when no doctype was written. `external_subset_omitted` and
 *   `output_reduced` report an omitted top-level external identifier.
 *   `references_written` counts character references generated for XML 1.1
 *   restricted characters or characters unavailable in the output encoding.
 * - A template element's content, being no child of the element, is refused
 *   rather than dropped.
 *
 * ## XHTML-compatible output
 *
 * With GTEXT_XML_Write_Options::xhtml the writer produces XML that an HTML
 * parser reads, as `text/html`, to the same tree an XML parser reads from it as
 * `application/xhtml+xml`. **The same bytes are two different trees depending
 * on the media type**, and nothing here guesses which one is meant: the parser
 * is always chosen by the caller, and no function in this library sniffs
 * content for an "HTML-ness".
 *
 * Only elements in the XHTML namespace are treated so; everything else is
 * written as XML. For those elements:
 *
 * - an empty void element (`br`, `img`, ...; markup_html.h) is `<br />`, with
 *   the space; every other empty element is expanded (`<p></p>`,
 *   `<script></script>`, never `<script/>`, which HTML reads as an open tag);
 * - the XHTML namespace is declared on the root element when the root is an
 *   XHTML element and has no declaration (as `declare_namespaces` would, without
 *   needing it);
 * - a boolean attribute is written with the value the tree holds, never
 *   rewritten; `script` and `style` content is written without a CDATA section
 *   and with `>` unescaped; `pre`, `listing`, `textarea`, `script`, `style` and
 *   `title` are never indented.
 *
 * Refused (GTEXT_XML_E_XHTML, with the path and the reason), not repaired:
 * a void element with children; `<`, `&`, `]]>` or a carriage return in
 * `script` or `style` text; a `pre`, `textarea` or `listing` whose text starts
 * with a line feed (HTML drops the first one, XML keeps it); a comment that
 * starts with `>` or `->` (HTML ends it early; one ending in `<!-` is refused
 * already, as GTEXT_XML_E_UNREPRESENTABLE, because it ends in `-`); a processing
 * instruction (HTML makes it a comment); a CDATA section outside SVG and MathML
 * content; an XHTML element with a prefix (HTML reads `h:p` as a different
 * element); an XHTML element or attribute name with an ASCII upper case letter
 * (HTML lower-cases it, so `BR` is not void and `<BR></BR>` is two tags), and an
 * XHTML element name that does not start with an ASCII letter, and a character
 * from U+0080 to U+009F: under US-ASCII it would be a numeric reference, which an
 * HTML parser reads as the Windows-1252 character at that code, and under ISO-8859-1
 * the parser reads the label as Windows-1252, so the raw byte means that character too.
 *
 * Tree-construction hazards (GTEXT_XML_Xhtml_Hazard) cannot be fixed by
 * spelling and are reported in the result as a count and the first one's kind
 * and path; with GTEXT_XML_Write_Options::xhtml_strict they are refusals. The
 * hazards are the ones the differential against html5lib and parse5 has found:
 * "no hazards" is measured, not a proof that an HTML parser reads the same tree
 * (see GTEXT_XML_Xhtml_Hazard).
 *
 * The XML declaration is written under the usual rules; an HTML parser reads it
 * as a comment, so use GTEXT_XML_DECLARATION_NEVER for output meant for both.
 * The guarantee for the XML side is the round trip: for every tree the mode
 * accepts, parsing the output with the XML parser gives an equal tree, subject
 * to the DTD mode, identifier policy, and equivalent external resources
 * described above. The
 * HTML-parser half is checked against html5lib and parse5 (`make
 * check-html-oracle`; see notes/text/XML-HTML-PLAN.md), and will be against this
 * library's own HTML parser when there is one. The mode never applies a
 * `<meta charset>`: the references are given characters.
 */

#ifndef GHOTI_IO_GTEXT_XML_XML_WRITER_H
#define GHOTI_IO_GTEXT_XML_XML_WRITER_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <ghoti.io/text/markup/markup_tree.h>
#include <ghoti.io/text/xml/xml_core.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_XML_Output_Encoding
 * @brief The encoding of the bytes written.
 */
typedef enum {
  GTEXT_XML_OUTPUT_UTF8 = 0,    ///< UTF-8, no byte order mark. The default.
  GTEXT_XML_OUTPUT_UTF16LE,     ///< UTF-16, little endian, with a byte order mark.
  GTEXT_XML_OUTPUT_UTF16BE,     ///< UTF-16, big endian, with a byte order mark.
  GTEXT_XML_OUTPUT_ISO_8859_1,  ///< ISO-8859-1; other characters as references.
  GTEXT_XML_OUTPUT_US_ASCII     ///< US-ASCII; other characters as references.
} GTEXT_XML_Output_Encoding;

/**
 * @enum GTEXT_XML_Declaration_Mode
 * @brief Whether to write `<?xml ...?>`.
 */
typedef enum {
  /**
   * Write one when the document has one (it was parsed with one, or its version
   * or standalone flag was set), or when the output is ISO-8859-1, which can
   * hold bytes that are not UTF-8 and so cannot be read back without saying so
   * (US-ASCII output is UTF-8, and UTF-16 has its byte order mark). The default.
   */
  GTEXT_XML_DECLARATION_AUTO = 0,
  GTEXT_XML_DECLARATION_ALWAYS, ///< Always. A document with no version gets "1.0".
  GTEXT_XML_DECLARATION_NEVER   ///< Never. Refused for an XML 1.1 tree because
                                ///< omission makes readers select XML 1.0.
                                ///< With ISO-8859-1 output a character from
                                ///< U+0080 to U+00FF is also refused
                                ///< (GTEXT_XML_E_UNREPRESENTABLE): the bytes
                                ///< would not read back.
} GTEXT_XML_Declaration_Mode;

/**
 * @enum GTEXT_XML_Bom_Mode
 * @brief Whether UTF-8 output starts with a byte order mark (EF BB BF).
 *
 * Only UTF-8 output has a choice: UTF-16 output always starts with its mark
 * (GTEXT_XML_Write_Options::encoding), and ISO-8859-1 and US-ASCII have no such
 * mark.
 */
typedef enum {
  /**
   * Write one when the output is UTF-8 and the document was parsed with one:
   * its recorded encoding is UTF-8 and the source of that is a byte order mark
   * (gtext_markup_document_encoding()). So parse(file with a mark) then write
   * gives a file with one, and a document without gives none. The default.
   */
  GTEXT_XML_BOM_AUTO = 0,
  /** Write one for UTF-8 output, whatever the document was. For UTF-16 it is
   *  what is written anyway; for ISO-8859-1 and US-ASCII there is no such mark and
   *  the write is refused as GTEXT_XML_E_INVALID. */
  GTEXT_XML_BOM_ALWAYS,
  /** Never write one for UTF-8 output. UTF-16 output keeps its mark: the
   *  encoding is unreadable without it. */
  GTEXT_XML_BOM_NEVER
} GTEXT_XML_Bom_Mode;

/**
 * @struct GTEXT_XML_Write_Options
 * @brief What a write may do. Zeroed is the same as the defaults.
 */
typedef struct {
  /**
   * The allocator for everything the writer allocates (its working state, and
   * the buffer of gtext_xml_write_buffer()), or NULL for the document's own.
   * The struct is copied; the `ctx` it holds must outlive the call.
   */
  const GTEXT_Allocator * allocator;

  /** The XML declaration. Its version and standalone flag come from the
   *  document. Only version "1.0" can be written: any other is
   *  GTEXT_XML_E_UNSUPPORTED_VERSION in every mode, including NEVER. Its
   *  encoding is the output encoding. */
  GTEXT_XML_Declaration_Mode xml_declaration;

  /** The output encoding. UTF-16 is written with a byte order mark and
   *  declared as "UTF-16". */
  GTEXT_XML_Output_Encoding encoding;

  /** Write an element with no children as `<a></a>` instead of `<a/>`. */
  bool expand_empty_elements;

  /**
   * Add the `xmlns` declarations a name needs instead of refusing it: on the
   * element whose own name or attribute needs it, after its other attributes. A
   * prefix an element's own name relies on is never rebound for an attribute,
   * and an attribute in a namespace with no prefix is still refused (inventing
   * a prefix would write a different name). Counted in
   * GTEXT_XML_Write_Result::namespace_declarations_added.
   */
  bool declare_namespaces;

  /**
   * NULL for compact output. Otherwise one level of indentation: spaces and
   * tabs only ("" is allowed: line breaks and no indentation).
   */
  const char * indent;

  /** What a line break is when pretty printing and between top-level items:
   *  a non-empty string of line feeds and carriage returns, or NULL for "\n". */
  const char * newline;

  /** Deepest element nesting, the root element being depth 1, or 0 for the
   *  default of 256. Exceeding it is GTEXT_XML_E_LIMIT_DEPTH. */
  size_t max_depth;

  /** Most bytes of output (the byte order mark included), or 0 for the default
   *  of 256 MiB. Exceeding it is GTEXT_XML_E_LIMIT_OUTPUT, before any byte is
   *  written. */
  size_t max_output_bytes;

  /** A UTF-8 byte order mark. See GTEXT_XML_Bom_Mode. It counts toward
   *  `max_output_bytes`, as UTF-16's does. Appended so that zeroed is the default. */
  GTEXT_XML_Bom_Mode byte_order_mark;

  /**
   * XHTML-compatible output: the XML an HTML parser (`text/html`) reads to the
   * same tree as an XML parser (`application/xhtml+xml`). See the file comment.
   * Appended, so zeroed is off and the output without it is byte-identical to
   * what it was before the option existed.
   */
  bool xhtml;

  /** With `xhtml`: a tree-construction hazard
   *  (GTEXT_XML_Write_Result::xhtml_hazards) is a refusal,
   *  GTEXT_XML_E_XHTML, not a count. Without `xhtml` it is
   *  GTEXT_XML_E_INVALID. */
  bool xhtml_strict;

  /** Maximum parser work used to check a retained internal DTD subset, or 0
   *  for the default of 64 MiB. Exceeding it is GTEXT_XML_E_LIMIT_WORK before
   *  any output is written. This bounds the synthetic input and parser work;
   *  the parser's separate declaration, expansion, reference, and nesting
   *  safeguards remain in force and report their own limit statuses. Appended
   *  so zeroed remains the default. */
  size_t max_dtd_work;

  /** Emit retained external identifiers in the doctype and internal DTD
   *  declarations. Default false: the top-level external subset identifier is
   *  omitted, and internal external-entity/notation declarations are refused.
   *  When true, identifiers are written as stored; the writer never resolves or
   *  fetches their resources. Appended so zeroed remains the default. */
  bool emit_external_identifiers;
} GTEXT_XML_Write_Options;

/**
 * @brief The defaults, spelled out.
 *
 * @return Allocator NULL, declaration AUTO, UTF-8, `<a/>`, no namespace
 *   declarations added, compact, "\n", depth 256, 256 MiB output, 64 MiB
 *   retained-DTD preflight work, and external identifiers disabled.
 */
GTEXT_API GTEXT_XML_Write_Options gtext_xml_write_options_default(void);

/**
 * @enum GTEXT_XML_Xhtml_Hazard
 * @brief What an `xhtml` write found that an HTML parser would build differently
 *   and that no spelling can prevent.
 *
 * **This is a measured list, not a proof.** A result with no hazards means the tree
 * is free of the hazards listed here. Over every tree tried in the differential
 * against html5lib 1.1 and parse5 8.0.1 (tools/oracle/html_diff.py, `make
 * check-html-oracle`: generated trees, the W3C XML suite, the standard's own
 * trees) such a tree was read as the tree written, except where html5lib 1.1
 * alone predates the standard (`template`, an `hr` in a `select`, ruby parts,
 * `command`); every divergence the differential found extended this list or the
 * refusals. The list is conservative where it models HTML's scope rules (it does
 * not know about `button`, `td` or `template` boundaries) and where the
 * references are behind the standard, and it is incomplete where the references
 * cannot show it.
 */
typedef enum {
  GTEXT_XML_XHTML_HAZARD_NONE = 0,
  /** A `table` (or `tbody`, `thead`, `tfoot`) with a `tr`, `td`, `th` or `col`
   *  child one level too high: HTML inserts the `tbody`, `tr` or `colgroup`. */
  GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART,
  /** Text that is not white space, or an element that does not belong, directly
   *  in a `table`, `tbody`, `thead`, `tfoot` or `tr`: HTML moves it in front
   *  of the table (foster parenting). */
  GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT,
  /** A block-level element inside a `p` (at any depth): HTML ends the `p`. */
  GTEXT_XML_XHTML_HAZARD_P_BLOCK,
  /** An `a` inside an `a`. */
  GTEXT_XML_XHTML_HAZARD_NESTED_A,
  /** A `form` inside a `form`. */
  GTEXT_XML_XHTML_HAZARD_NESTED_FORM,
  /** A `li`, `dt`/`dd` that HTML ends where the next starts (reached through
   *  the ancestor chain, so `li > div > li` counts), or an `option`,
   *  `optgroup`, heading, `tr`, `td`/`th` or table section directly in one
   *  that HTML ends. */
  GTEXT_XML_XHTML_HAZARD_IMPLIED_END,
  /** Markup (an element, comment) inside `script`, `style`, `title` or
   *  `textarea`, or `<`, `&`, `>` or a carriage return in the text of
   *  `iframe`, `noembed`, `noframes`, `noscript`, `plaintext` or `xmp`: HTML
   *  reads it as text, and the escapes the writer needs are shown literally. */
  GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT,
  /** The document is not `html` with exactly a `head` and then a `body`
   *  (HTML inserts what is missing), has a nested `html`, `head` or `body`,
   *  has text directly in `html`, or has in `head` something that ends the
   *  head. */
  GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE,
  /** An XHTML-namespace element HTML rewrites or treats specially: `image`,
   *  `isindex`, `frameset`, `frame`, `svg`, `math`; a `template` with children
   *  (they go to a content fragment); a `button` in a `button`; a `colgroup`
   *  with children other than `col`; a `select` with children other than
   *  `option`, `optgroup`, `hr`, `script`, `template`; a table part (`caption`,
   *  `colgroup`, `col`, `tbody`, `tr`, `td`, ...) not in a table, which HTML
   *  ignores. */
  GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT,
  /** SVG or MathML content HTML would build differently: an element in
   *  those namespaces with a prefix, not inside an `svg` or `math` element, in
   *  a different foreign namespace than its parent, or named like an HTML tag
   *  that breaks out of foreign content (`div`, `p`, `font color`, ...); an
   *  HTML element directly inside foreign content that is not an integration
   *  point; an element in any other namespace; an element or attribute name
   *  with an upper case letter that HTML would lower-case and does not restore
   *  (gtext_markup_html_foreign_name_survives()), or a namespaced attribute that
   *  is not one of the nine HTML reads as namespaced (`xlink:href`, `xml:lang`, ...).
   *  Markup other than text inside `iframe`, `noembed`, `noframes`,
   *  `noscript`, `plaintext` or `xmp` is RAW_TEXT_CONTENT, not this. */
  GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT,
  /** An attribute with a namespace on an XHTML element (`xml:lang`,
   *  `xlink:href`): HTML reads it as a plain attribute of that prefixed name.
   *  Also a namespace declaration other than on the root element or the
   *  default declaration of the XHTML namespace itself. */
  GTEXT_XML_XHTML_HAZARD_NAMESPACED_ATTRIBUTE,
  /** The serialized doctype is not `<!DOCTYPE html>` without an internal
   *  subset (HTML derives quirks mode from it and does not support subsets). */
  GTEXT_XML_XHTML_HAZARD_DOCTYPE
} GTEXT_XML_Xhtml_Hazard;

/**
 * @struct GTEXT_XML_Write_Result
 * @brief Why a write failed and where, and what it did.
 *
 * Cleared at the start of every call that takes one. The counts are meaningful
 * on success and are what was seen before the failure otherwise.
 */
typedef struct {
  GTEXT_XML_Status code;   ///< The status; GTEXT_XML_OK if nothing failed.
  const char * message;    ///< A static description. Never freed.
  /**
   * Where, as a path from the document: `/a/b[2]/@x`, `/a/comment()`. An
   * element is its name as written, with an index among the same-named siblings
   * when there is more than one; the tail is kept when the path is long (it
   * then starts with `...`). Empty when no node is at fault.
   */
  char path[256];
  char name[96];           ///< The prefix, name or target at fault, if any.

  size_t bytes_written;    ///< Bytes of output.
  size_t elements_indented; ///< Elements whose children were put on lines of
                           ///< their own.
  size_t elements_skipped_mixed; ///< Elements with children left as they were
                           ///< because they, or an ancestor, hold text or CDATA.
  size_t elements_skipped_preserve; ///< Elements with children left as they were
                           ///< because of `xml:space="preserve"` on them or above.
  size_t references_written; ///< Generated character references, for XML 1.1
                             ///< restricted characters or characters the
                             ///< output encoding cannot represent.
  size_t namespace_declarations_added; ///< By GTEXT_XML_Write_Options::declare_namespaces.
  size_t text_nodes_coalesced; ///< Empty text nodes, and text nodes that follow
                           ///< another: XML reads them back as one node or none.
  bool doctype_without_subset; ///< A doctype was written without a retained
                           ///< internal subset. False when retained syntax was
                           ///< emitted in its original order.
  bool output_reduced; ///< True when external identifiers were omitted.
  bool external_subset_omitted; ///< A top-level external subset identifier
                           ///< was omitted by the default writer policy.
  size_t xhtml_hazards;    ///< `xhtml` only: how many hazards were found
                           ///< (GTEXT_XML_Xhtml_Hazard). With `xhtml_strict` the
                           ///< first one is the refusal, so this is 1.
  GTEXT_XML_Xhtml_Hazard xhtml_hazard_kind; ///< The kind of the first one.
  char xhtml_hazard_path[256]; ///< Its path, in the form of `path`; a path
                           ///< longer than 255 bytes keeps its end behind `...`
                           ///< (truncated, not wrapped). Empty when there is
                           ///< none.
  size_t unresolved_references_written; ///< Unresolved entity-reference nodes
                           ///< emitted as `&name;`; zero when none were
                           ///< emitted.
  bool internal_subset_emitted; ///< Retained internal DTD syntax was emitted.
} GTEXT_XML_Write_Result;

/**
 * @brief Write a document to a sink.
 *
 * See the file comment for what is refused and for the round trip. The sink
 * receives the output in chunks of up to 4 KiB, cut anywhere (a chunk may end
 * inside a character); a non-zero return from it
 * stops the write with GTEXT_XML_E_WRITE, or GTEXT_XML_E_OOM when it returned
 * GTEXT_MARKUP_E_OOM.
 *
 * @param sink   Where the bytes go. Not NULL, and `write` not NULL.
 * @param doc    The document. Must have a root element.
 * @param opts   Options, or NULL for gtext_xml_write_options_default().
 * @param result Receives the outcome, or NULL.
 * @return GTEXT_XML_OK, or why not: GTEXT_XML_E_INVALID for a bad argument or
 *   option, GTEXT_XML_E_OOM, GTEXT_XML_E_WRITE, GTEXT_XML_E_LIMIT_DEPTH,
 *   GTEXT_XML_E_LIMIT_OUTPUT, GTEXT_XML_E_UNSUPPORTED_VERSION, and the statuses
 *   for a tree that cannot be written: GTEXT_XML_E_UNREPRESENTABLE,
 *   GTEXT_XML_E_CHARACTER, GTEXT_XML_E_NAME, GTEXT_XML_E_NAMESPACE,
 *   GTEXT_XML_E_DOCTYPE. Nothing is leaked on any of them.
 */
GTEXT_API GTEXT_XML_Status gtext_xml_write(const GTEXT_Markup_Sink * sink,
    const GTEXT_Markup_Document * doc, const GTEXT_XML_Write_Options * opts,
    GTEXT_XML_Write_Result * result);

/**
 * @brief Write a document to a new buffer.
 *
 * @param doc     The document.
 * @param opts    Options, or NULL for the defaults.
 * @param out     Receives the bytes, followed by one NUL byte not counted in
 *                `out_len` (for UTF-16 the output is not text and has NULs in it:
 *                use `out_len`). Release with gtext_xml_write_buffer_free().
 *                Written only on success.
 * @param out_len Receives the length. May be NULL. Untouched on failure, as
 *                `out` is.
 * @param result  Receives the outcome, or NULL.
 * @return As gtext_xml_write(); a failure leaves no buffer.
 */
GTEXT_API GTEXT_XML_Status gtext_xml_write_buffer(
    const GTEXT_Markup_Document * doc, const GTEXT_XML_Write_Options * opts,
    char ** out, size_t * out_len, GTEXT_XML_Write_Result * result);

/**
 * @brief Release a buffer from gtext_xml_write_buffer().
 *
 * The buffer belongs to the allocator it was written with: `opts->allocator`
 * if the write's options had one, else the document's. Pass the same `opts`
 * (or at least the same allocator) as the write. `doc` may be NULL only when
 * `opts->allocator` is given; with neither, nothing is freed.
 *
 * @param doc    The document it was written from, or NULL (see above).
 * @param opts   The options it was written with, or NULL.
 * @param buffer The buffer. NULL is ignored.
 */
GTEXT_API void gtext_xml_write_buffer_free(const GTEXT_Markup_Document * doc,
    const GTEXT_XML_Write_Options * opts, char * buffer);

/**
 * @brief Write a document to a file, replacing it atomically.
 *
 * The content goes to a temporary file beside the destination and replaces it
 * only when complete, so a write that fails (a refused tree included) leaves
 * the previous file as it was. This is the file layer the CSV and JSON file
 * functions use, and it reports the same way: a path that cannot be opened is
 * GTEXT_XML_E_INVALID, a failed write GTEXT_XML_E_WRITE, and a failure of the
 * serializer is its own, more specific, status.
 *
 * @param path   The file to write.
 * @param doc    The document.
 * @param opts   Options, or NULL for the defaults.
 * @param result Receives the outcome, or NULL.
 * @return GTEXT_XML_OK or why not, as gtext_xml_write().
 */
GTEXT_API GTEXT_XML_Status gtext_xml_write_file(const char * path,
    const GTEXT_Markup_Document * doc, const GTEXT_XML_Write_Options * opts,
    GTEXT_XML_Write_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_XML_XML_WRITER_H
