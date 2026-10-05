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
 * @file yaml_stream.h
 * @brief Streaming (event-driven) YAML parser API.
 *
 * The streaming API accepts chunks of input and emits events via a
 * user-provided callback. This allows parsing large or incremental
 * inputs without needing to build a full DOM.
 */

#ifndef GHOTI_IO_GTEXT_YAML_YAML_STREAM_H
#define GHOTI_IO_GTEXT_YAML_YAML_STREAM_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/yaml/yaml_core.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef struct GTEXT_YAML_Stream GTEXT_YAML_Stream;
typedef struct GTEXT_YAML_Reader GTEXT_YAML_Reader;

/**
 * @brief Event callback invoked by the streaming parser.
 *
 * The callback receives the parser instance @p s, a pointer to an
 * event-specific payload (opaque here), and the user-provided context.
 * The callback should return a @ref GTEXT_YAML_Status; returning a
 * non-zero status will abort parsing and be propagated to the caller.
 */
typedef GTEXT_YAML_Status (*GTEXT_YAML_Event_Callback)(GTEXT_YAML_Stream * s, const void * event_payload, void * user);

/**
 * @enum GTEXT_YAML_Event_Type
 * @brief Event kinds emitted by the streaming parser.
 */
typedef enum {
	GTEXT_YAML_EVENT_STREAM_START,
	GTEXT_YAML_EVENT_STREAM_END,
	GTEXT_YAML_EVENT_DOCUMENT_START,
	GTEXT_YAML_EVENT_DOCUMENT_END,
	GTEXT_YAML_EVENT_DIRECTIVE,
	GTEXT_YAML_EVENT_COMMENT,
	GTEXT_YAML_EVENT_SEQUENCE_START,
	GTEXT_YAML_EVENT_SEQUENCE_END,
	GTEXT_YAML_EVENT_MAPPING_START,
	GTEXT_YAML_EVENT_MAPPING_END,
	GTEXT_YAML_EVENT_SCALAR,
	GTEXT_YAML_EVENT_ALIAS,
	GTEXT_YAML_EVENT_INDICATOR,
} GTEXT_YAML_Event_Type;

/**
 * @struct GTEXT_YAML_Event
 * @brief Payload for streaming events.
 *
 * For scalar events the `scalar` field is valid. For indicator events,
 * the `indicator` field holds the single-character indicator.
 * 
 * The `anchor` field contains the anchor name if this event has an anchor
 * (from & marker), otherwise NULL. The `tag` field contains the tag if 
 * this event has an explicit tag (from !! marker), otherwise NULL.
 * 
 * For ALIAS events, the `alias_name` field contains the referenced anchor name.
 */
typedef struct {
	GTEXT_YAML_Event_Type type; ///< What kind of event this is
	union {
		struct {
			const char * ptr;
			size_t len;
		} scalar;
		struct {
			const char * name;
			const char * value;
			const char * value2;
		} directive;
		struct {
			const char * ptr;
			size_t len;
			bool inline_comment;
		} comment;
		const char * alias_name;  /* For GTEXT_YAML_EVENT_ALIAS */
		char indicator;
	} data; ///< The payload, selected by `type`
	const char * anchor;  ///< Anchor name for this node (NULL if none)
	const char * tag;     ///< Explicit tag for this node (NULL if none)
	/** A second anchor or tag, written before the one above and on an earlier
	 * line, whose node is not this one but the block collection this node
	 * opens - if it opens one.
	 *
	 * A node carries at most one anchor and at most one tag (7.1), so two of
	 * a kind with nothing between them name two nodes, and in block context
	 * the only thing that can stand between them is a collection that starts
	 * where the second was written:
	 *
	 *     top1: &node1        &node1 is the mapping's, &k1 the key's, and
	 *       &k1 key1: val1    they arrive together on the key
	 *
	 * The stream cannot tell whether that collection opens until the token
	 * after the node, so it reports both and leaves the question to the
	 * consumer. If no collection opens the two named one node after all,
	 * which is an error - suite case 4JVG:
	 *
	 *     top2: &node2
	 *       &v2 val2
	 *
	 * NULL, with a line of 0, when there is no such property. */
	const char * outer_anchor; ///< The earlier anchor, or NULL
	int outer_anchor_line;     ///< 1-based line `outer_anchor` was written on, or 0
	const char * outer_tag;    ///< The earlier tag, or NULL
	int outer_tag_line;        ///< 1-based line `outer_tag` was written on, or 0
	/** 1-based line the tag was written on, or 0 when there is no tag.
	 *
	 * A tag applies to the node that follows it, and in block context the
	 * parser cannot tell which node that is until it has seen what comes
	 * next.  "!custom a: 1" tags the key; "!custom" then "a: 1" on the next
	 * line tags the mapping.  The two produce the same events and differ
	 * only in where the tag was written, so the position has to travel with
	 * it. */
	int tag_line;
	/** 1-based line the anchor was written on, or 0 when there is no anchor.
	 *
	 * The same problem as tag_line, and the same answer. An anchor applies to
	 * the node that follows it, and in block context the parser cannot tell
	 * which node that is until it has seen what comes next: "&a key: 1"
	 * anchors the key, while "&a" on its own line and then "key: 1" anchors
	 * the mapping. The two produce the same events and differ only in where
	 * the anchor was written.
	 *
	 * prop_line below will not answer this. It is the leftmost property's
	 * line, and with an anchor on one line and a tag further left on the
	 * next, the leftmost is the tag - so an anchor written before the
	 * collection would look as though it shared the node's line. */
	int anchor_line;
	/** Where the leftmost property of this node was written: 1-based line and
	 * 0-based column, or 0 and -1 when the node carries no properties.
	 *
	 * A property has to be indented past the block collection that is already
	 * open, because c-ns-properties appears in s-l+block-collection(n,c) only
	 * after s-separate(n+1,c) (8.2).  The parser cannot use the column of the
	 * node itself to check that: by the time a scalar arrives, the collection
	 * its properties introduced may already have been opened at a deeper
	 * indentation.  The leftmost is the one that binds, since every property
	 * of the node has to clear the same indentation.
	 *
	 * For example, "&node" on a line of its own between two "- " entries is
	 * at the sequence's own indentation, so it introduces nothing and the
	 * document is in error - even though the scalar it ends up attached to
	 * sits further in.
	 *
	 * On an INDICATOR event there is no node yet, and these say instead where
	 * a property that has not found its node was written - 0 and -1 when
	 * none is waiting.  A consumer deciding what the indicator opens can then
	 * see that something is already waiting to name it. */
	int prop_line; ///< 1-based line of the leftmost property, or 0 (see above)
	int prop_col;  ///< 0-based column of the leftmost property, or -1
	GTEXT_YAML_Scalar_Style scalar_style; ///< Preferred scalar style (SCALAR events)
	/** DOCUMENT_START and DOCUMENT_END only: the marker was written.
	 *
	 * Both events fire for every document, whether or not the stream says so
	 * in as many characters - a bare "a: 1" is a document, and a stream that
	 * runs out of input ends the one that was open.  The two that were
	 * written are not the same fact as the two that were inferred, and only
	 * the writer's are recoverable afterwards, so the event carries which it
	 * is: true for a DOCUMENT_START produced by "---" and for a DOCUMENT_END
	 * produced by "...", false for either supplied by the stream.
	 *
	 * "---" closing a document that had no "..." is an inferred end followed
	 * by a written start, and reports itself that way. */
	bool explicit_marker;
	/** Where this event is. @ref gtext_yaml_offsets applies: offset
	 *  counts bytes of the decoded stream, which is an index into the
	 *  caller's buffer only for UTF-8 with no byte order mark. */
	size_t offset;
	int line; ///< Line of the event
	int col;  ///< Column of the event
} GTEXT_YAML_Event;

/**
 * @brief Create a new streaming parser.
 *
 * The caller may pass NULL for @p opts to use library defaults. The
 * provided callback @p cb will be called for each parser event. Returns
 * a heap-allocated parser object which must be freed with
 * @ref gtext_yaml_stream_free().
 */
GTEXT_API GTEXT_YAML_Stream * gtext_yaml_stream_new(const GTEXT_YAML_Parse_Options * opts, GTEXT_YAML_Event_Callback cb, void * user);

/**
 * @brief Feed a chunk of input to the streaming parser.
 *
 * The parser accepts arbitrary chunk boundaries; it may buffer input
 * internally if a token or block scalar spans multiple calls. Returns
 * GTEXT_YAML_OK on success or an error status. If the callback returns
 * an error, that status will be returned from this function.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_stream_feed(GTEXT_YAML_Stream * s, const char * data, size_t len);

/**
 * @brief Notify the parser that no more input will arrive and finish parsing.
 *
 * This allows the parser to validate final state (e.g., unclosed
 * block scalars) and emit remaining events. Returns GTEXT_YAML_OK on
 * success or an error status.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_stream_finish(GTEXT_YAML_Stream * s);

/**
 * @brief Free the streaming parser and its internal buffers.
 *
 * Passing NULL is a no-op.
 */
GTEXT_API void gtext_yaml_stream_free(GTEXT_YAML_Stream * s);

/**
 * @brief Create a new pull-model YAML reader.
 *
 * The pull reader wraps the streaming parser and queues events for
 * synchronous consumption via gtext_yaml_reader_next().
 */
GTEXT_API GTEXT_YAML_Reader * gtext_yaml_reader_new(
	const GTEXT_YAML_Parse_Options * opts
);

/**
 * @brief Feed input to the pull reader.
 *
 * To signal end-of-input, call with data=NULL and len=0, which will
 * finalize parsing and enqueue any remaining events.
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_reader_feed(
	GTEXT_YAML_Reader * reader,
	const void * data,
	size_t len,
	GTEXT_YAML_Error * out_err
);

/**
 * @brief Retrieve the next available event from the reader.
 *
 * Returns GTEXT_YAML_OK when an event is available, GTEXT_YAML_E_INCOMPLETE
 * if more input is needed, or GTEXT_YAML_E_STATE if the stream has ended.
 *
 * The returned event's string pointers remain valid until the next
 * call to gtext_yaml_reader_next() or gtext_yaml_reader_free().
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_reader_next(
	GTEXT_YAML_Reader * reader,
	GTEXT_YAML_Event * out_event,
	GTEXT_YAML_Error * out_err
);

/**
 * @brief Free the pull reader and any queued events.
 */
GTEXT_API void gtext_yaml_reader_free(GTEXT_YAML_Reader * reader);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_YAML_YAML_STREAM_H
