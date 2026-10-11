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
 * @file xml_stream.h
 * @brief Parse XML as a stream of events, pushed or pulled.
 *
 * The same parser as gtext_xml_parse() - the same well-formedness verdicts,
 * limits, namespaces, encodings and positions - for documents that arrive in
 * pieces, are too large to want as a tree, or are not wanted as a tree at all.
 * It has the shape of csv_stream.h: a push parser that calls the caller
 * (GTEXT_XML_Stream) and a pull reader that the caller calls
 * (GTEXT_XML_Reader).
 *
 * ## Chunk boundaries do not matter
 *
 * A document fed whole, a byte at a time, or cut anywhere else yields the same
 * events with the same bytes and the same locations. Character data is
 * **coalesced**: it is delivered as one TEXT event when the next piece of markup
 * begins, however many pieces it arrived in and however many references it
 * contains. A construct is never cut: a start tag, a comment, a CDATA section
 * and the rest are each delivered whole, once all of it has arrived.
 *
 * An event is therefore delivered when the *next* byte that decides it has
 * arrived, or at gtext_xml_stream_finish(); text before the end of a document
 * is held until a `<` follows it.
 *
 * ## Memory and time
 *
 * Memory is bounded by the largest single construct in the document (a text run
 * is one construct: it is held until it ends) and not by the size of the
 * document. Bytes that have been read are dropped. **A construct that never ends
 * is bounded only by `max_input_bytes`**: an unterminated start tag, comment or
 * processing instruction is held, with no event and no error, until it ends, the
 * input ends, or the total fed passes that limit. The name, attribute and
 * namespace limits apply when the construct is read, not while it is waited
 * for. A caller that streams should therefore set `max_input_bytes` to what it
 * can hold; note that the 64 MiB default also caps the size of the whole
 * document. The one exception is an XML declaration, which is waited for up to
 * 64 KiB and then refused with GTEXT_XML_E_SYNTAX. Work is linear in the
 * input: the scan for the end of a construct resumes where it stopped, so a
 * megabyte comment fed a byte at a time costs a megabyte of scanning, not a
 * megabyte squared.
 *
 * ## Errors
 *
 * A document that is not well-formed is reported from the call that supplied the
 * byte that settled it - gtext_xml_stream_feed(), or gtext_xml_stream_finish()
 * for one that ends too soon - with the status and position gtext_xml_parse()
 * would give. Two verdicts depend on how much has been seen and are reported at
 * the call that sees it: a mistake early in a construct is found when the
 * construct is complete (or at finish), not at the byte itself, and bytes that
 * are invalid in a UTF-16, UTF-32, ISO-8859-1 or US-ASCII document are reported
 * as they arrive, so a syntax error earlier in the document is reported if it
 * was found first. gtext_xml_parse() converts the whole input before reading it
 * and so reports the invalid bytes in preference to an earlier syntax error;
 * the stream reports whichever comes first. Events for what came before an
 * error have already been delivered.
 *
 * Once any call has failed - the document is not well-formed, a limit was
 * reached, memory ran out, or the callback returned an error - the parser is
 * finished: every later call returns the same error.
 */

#ifndef GHOTI_IO_GTEXT_XML_XML_STREAM_H
#define GHOTI_IO_GTEXT_XML_XML_STREAM_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_core.h>
#include <ghoti.io/text/xml/xml_core.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_XML_Event_Type
 * @brief What an event is.
 *
 * A document is DOCUMENT_START, then (in the prolog) any DOCTYPE, COMMENT and
 * PROCESSING_INSTRUCTION, then the root element and everything in it, then more
 * COMMENT and PROCESSING_INSTRUCTION events, then DOCUMENT_END. White space
 * outside the root element is not an event. An element is ELEMENT_START, its
 * content, ELEMENT_END; an empty-element tag `<a/>` is the pair with nothing
 * between.
 */
typedef enum {
  GTEXT_XML_EVENT_DOCUMENT_START, ///< The XML declaration, or its absence.
  GTEXT_XML_EVENT_DOCTYPE,        ///< The document type declaration.
  GTEXT_XML_EVENT_ELEMENT_START,  ///< A start tag, or an empty-element tag.
  GTEXT_XML_EVENT_ELEMENT_END,    ///< An end tag, or the end of an empty one.
  GTEXT_XML_EVENT_TEXT,           ///< Character data.
  GTEXT_XML_EVENT_CDATA,          ///< A CDATA section.
  GTEXT_XML_EVENT_COMMENT,        ///< A comment.
  GTEXT_XML_EVENT_PROCESSING_INSTRUCTION, ///< A processing instruction.
  GTEXT_XML_EVENT_DOCUMENT_END    ///< The end of a well-formed document.
} GTEXT_XML_Event_Type;

/**
 * @struct GTEXT_XML_Event_Attribute
 * @brief One attribute of a start tag, resolved.
 *
 * Namespace declarations are attributes here as they are in the tree: `xmlns="u"`
 * has no prefix and the local name `xmlns`; `xmlns:p="u"` has the prefix `xmlns`
 * and the local name `p`; both are in the namespace
 * `http://www.w3.org/2000/xmlns/`. An unprefixed attribute has no namespace
 * (the default namespace does not apply to attributes). Attributes are in source
 * order. The value has its line ends and references already normalized.
 */
typedef struct {
  const char * namespace_uri; ///< NULL for none.
  const char * prefix;        ///< NULL for none.
  const char * local_name;    ///< NUL-terminated.
  const char * value;         ///< NUL-terminated; never contains a NUL.
  size_t value_length;
  GTEXT_Markup_Location location; ///< Where the attribute's name begins.
} GTEXT_XML_Event_Attribute;

/**
 * @struct GTEXT_XML_Event
 * @brief One event. Which fields mean anything depends on `type`.
 *
 * Every pointer is valid **only during the callback** (or, for a reader, until
 * the next gtext_xml_reader_next()). Every string is NUL-terminated; a length is
 * given where the string may be long or hold NULs. `location` is where the
 * construct begins: the `<` for markup, the first character for text. Offsets
 * count bytes of the document as UTF-8 (see gtext_xml_parse() for what that
 * means for the other encodings), a byte order mark included for UTF-8.
 *
 * | type                    | fields |
 * |-------------------------|--------|
 * | DOCUMENT_START          | `version` (NULL if there is no XML declaration), `encoding`, `encoding_source`, `standalone` |
 * | DOCTYPE                 | `name`, `public_id`, `system_id` (NULL where absent) |
 * | ELEMENT_START           | `namespace_uri`, `prefix`, `local_name`, `attributes`, `attribute_count` |
 * | ELEMENT_END             | `namespace_uri`, `prefix`, `local_name` as the start had them; `location` is the end tag's `</`, or the `/` of an empty-element tag |
 * | TEXT, CDATA, COMMENT    | `data`, `data_length` |
 * | PROCESSING_INSTRUCTION  | `name` (the target), `data`, `data_length` |
 * | DOCUMENT_END            | none; `location` is the end of the document |
 */
typedef struct {
  GTEXT_XML_Event_Type type;
  GTEXT_Markup_Location location;

  /* ELEMENT_START, ELEMENT_END */
  const char * namespace_uri; ///< NULL for none.
  const char * prefix;        ///< NULL for none.
  const char * local_name;
  const GTEXT_XML_Event_Attribute * attributes; ///< ELEMENT_START only; NULL
                                                ///< when there are none.
  size_t attribute_count;

  /* TEXT, CDATA, COMMENT, PROCESSING_INSTRUCTION */
  const char * data;
  size_t data_length;

  /* DOCTYPE (name), PROCESSING_INSTRUCTION (target) */
  const char * name;
  const char * public_id;
  const char * system_id;

  /* DOCUMENT_START */
  const char * version;                      ///< "1.0", or NULL.
  const char * encoding;                     ///< The label as written, or the
                                             ///< name of what was detected
                                             ///< ("UTF-8", "UTF-16", ...).
  GTEXT_Markup_Encoding_Source encoding_source; ///< Why: BOM, declaration or
                                             ///< default.
  GTEXT_Markup_Standalone standalone;
} GTEXT_XML_Event;

/**
 * @brief Event callback.
 *
 * @param event     The event; valid only for the duration of the call.
 * @param user_data The pointer given to gtext_xml_stream_new().
 * @return GTEXT_XML_OK to continue, or any other status to stop: it is returned
 *   from the call that was parsing, and the parser is finished.
 */
typedef GTEXT_XML_Status (*GTEXT_XML_Event_cb)(
    const GTEXT_XML_Event * event, void * user_data);

/** @brief Opaque push parser. */
typedef struct GTEXT_XML_Stream GTEXT_XML_Stream;

/**
 * @brief Create a push parser.
 *
 * @param opts      Options (limits and allocator), or NULL for the defaults.
 *                  `max_input_bytes` counts all the bytes fed. `max_nodes`
 *                  counts the nodes gtext_xml_parse() would have built, the
 *                  document node included, so a document is refused by both or
 *                  by neither. The allocator is copied; the `ctx` it holds must
 *                  outlive the parser.
 * @param callback  Called for each event; must not be NULL. It must not call
 *                  back into this parser.
 * @param user_data Passed to the callback.
 * @return The parser, or NULL if `callback` is NULL or memory ran out.
 */
GTEXT_API GTEXT_XML_Stream * gtext_xml_stream_new(
    const GTEXT_XML_Parse_Options * opts, GTEXT_XML_Event_cb callback,
    void * user_data);

/**
 * @brief Give the parser more of the document.
 *
 * Delivers every event the bytes so far have completed. A `len` of 0 does
 * nothing, and `data` may then be NULL.
 *
 * @param stream The parser.
 * @param data   The next bytes of the document, in any encoding
 *               gtext_xml_parse() reads. A multi-byte character, a UTF-16
 *               code unit or a surrogate pair may be cut between calls.
 * @param len    Their number.
 * @param err    Receives the failure, or a cleared error; may be NULL.
 * @return GTEXT_XML_OK; why the document is refused; the callback's status;
 *   GTEXT_XML_E_STATE after gtext_xml_stream_finish(); or GTEXT_XML_E_INVALID
 *   for `data` NULL with a length.
 */
GTEXT_API GTEXT_XML_Status gtext_xml_stream_feed(GTEXT_XML_Stream * stream,
    const void * data, size_t len, GTEXT_XML_Error * err);

/**
 * @brief End the input.
 *
 * Delivers what the end completes and then DOCUMENT_END, or reports why the
 * document is not well-formed (an unterminated construct, an element still
 * open, no root element: GTEXT_XML_E_UNEXPECTED_EOF). Calling it again after it
 * succeeded does nothing and returns GTEXT_XML_OK.
 *
 * @param stream The parser.
 * @param err    Receives the failure, or a cleared error; may be NULL.
 * @return GTEXT_XML_OK, or why not.
 */
GTEXT_API GTEXT_XML_Status gtext_xml_stream_finish(
    GTEXT_XML_Stream * stream, GTEXT_XML_Error * err);

/**
 * @brief Free a push parser and everything it holds.
 *
 * Safe at any point, after a failure included, and for NULL.
 */
GTEXT_API void gtext_xml_stream_free(GTEXT_XML_Stream * stream);

/** @brief Opaque pull reader. */
typedef struct GTEXT_XML_Reader GTEXT_XML_Reader;

/**
 * @brief Create a pull reader.
 *
 * The push parser calls the caller; this lets the caller call the parser. It
 * wraps a GTEXT_XML_Stream and copies every event, with its strings, into a
 * queue, so an event stays valid until the next gtext_xml_reader_next() and not
 * only for a callback. The queue holds what one feed produced, so a document fed
 * in one piece is queued whole: feed in pieces to bound it.
 *
 * @param opts Options as for gtext_xml_stream_new(); the allocator covers the
 *             reader, its queue and the copies.
 * @return The reader, or NULL on allocation failure.
 */
GTEXT_API GTEXT_XML_Reader * gtext_xml_reader_new(
    const GTEXT_XML_Parse_Options * opts);

/**
 * @brief Feed the reader.
 *
 * `data` NULL with `len` 0 ends the input (and queues what the end completes,
 * DOCUMENT_END last); doing it twice is accepted. Bytes after the end are
 * GTEXT_XML_E_STATE. Any other status is the parse error, and is returned by
 * every later call.
 *
 * @param reader The reader.
 * @param data   The next bytes, or NULL to end the input.
 * @param len    Their number.
 * @param err    Receives the failure, or a cleared error; may be NULL.
 * @return GTEXT_XML_OK, or why not.
 */
GTEXT_API GTEXT_XML_Status gtext_xml_reader_feed(GTEXT_XML_Reader * reader,
    const void * data, size_t len, GTEXT_XML_Error * err);

/**
 * @brief Take the next event.
 *
 * @param reader    The reader.
 * @param out_event Receives the event.
 * @return GTEXT_XML_OK when there was an event; GTEXT_XML_E_INCOMPLETE when the
 *   reader needs more input; GTEXT_XML_E_STATE after the last event of a
 *   finished document; or the parse error, which every later call repeats so
 *   that a failure is never mistaken for the end. Events that were queued
 *   before the error are delivered first. The error's position is what the
 *   failing gtext_xml_reader_feed() reported.
 *
 * The event's pointers stay valid until the next call to this function or to
 * gtext_xml_reader_free().
 */
GTEXT_API GTEXT_XML_Status gtext_xml_reader_next(
    GTEXT_XML_Reader * reader, GTEXT_XML_Event * out_event);

/** @brief Free a reader, its queue and the event it last handed out. NULL is a no-op. */
GTEXT_API void gtext_xml_reader_free(GTEXT_XML_Reader * reader);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_XML_XML_STREAM_H
