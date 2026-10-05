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
 * @file yaml_events.h
 * @brief Walk a parsed document as a stream of composed node events.
 *
 * This is the *composed* event stream: one event per node of the document
 * tree, in the order the nodes were written, with each node's anchor, tag and
 * style attached to the node they belong to.  It is not the same thing as
 * @ref yaml_stream.h, whose @ref GTEXT_YAML_Event is shaped like the input -
 * indicators, comments, directives, and properties whose node is not yet
 * known.  A consumer that wants to know what the document *is* wants these
 * events; one that wants to know what the input *said* wants those.
 *
 * The walk reports the document as the DOM holds it, which is the document as
 * it was written with two exceptions, both from parse options that are on by
 * default:
 *
 * - Merge keys.  `allow_merge_keys` applies `<<` while the document is being
 *   resolved, so a mapping that used one is walked with the merge already
 *   performed and no `<<` pair in it.  Turn the option off to have the parser
 *   refuse such a document instead.
 * - Aliases are *not* expanded: an alias is its own event, carrying the
 *   anchor name, and the walk never follows it.  A document with a cycle in
 *   it therefore walks in finite time.
 */

#ifndef GHOTI_IO_GTEXT_YAML_YAML_EVENTS_H
#define GHOTI_IO_GTEXT_YAML_YAML_EVENTS_H

#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>

#include <ghoti.io/text/yaml/yaml_core.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_YAML_Node_Event_Type
 * @brief The kinds of event a walk produces.
 *
 * Every START has a matching END, and they nest.  A scalar and an alias are
 * single events with no END of their own.
 */
typedef enum {
  GTEXT_YAML_NODE_EVENT_STREAM_START,
  GTEXT_YAML_NODE_EVENT_STREAM_END,
  GTEXT_YAML_NODE_EVENT_DOCUMENT_START,
  GTEXT_YAML_NODE_EVENT_DOCUMENT_END,
  GTEXT_YAML_NODE_EVENT_SEQUENCE_START,
  GTEXT_YAML_NODE_EVENT_SEQUENCE_END,
  GTEXT_YAML_NODE_EVENT_MAPPING_START,
  GTEXT_YAML_NODE_EVENT_MAPPING_END,
  GTEXT_YAML_NODE_EVENT_SCALAR,
  GTEXT_YAML_NODE_EVENT_ALIAS
} GTEXT_YAML_Node_Event_Type;

/**
 * @struct GTEXT_YAML_Node_Event
 * @brief One event of a walk.
 *
 * Fields that do not apply to the event's type are zero, and the ones that do
 * are named below.  Strings point into the document's arena and stay valid
 * for as long as the document does; the struct itself does not outlive the
 * callback.
 */
typedef struct {
  GTEXT_YAML_Node_Event_Type type; ///< What kind of event this is

  /** The node this event is about.
   *
   * NULL for the four stream and document events, which are about no node,
   * and for a SCALAR the parser supplied rather than read: an omitted value
   * in `a:` and an omitted key in `: b` are the empty node (7.2), which is
   * a scalar with no text and no properties and nowhere in the tree to be. */
  const GTEXT_YAML_Node * node;

  /** The node's anchor, without the `&`, or NULL when it carries none. */
  const char * anchor;

  /** The node's tag as the node carries it, or NULL when it carries none.
   *
   * This is the tag with any `%TAG` handle already substituted: `!!str`,
   * `!foo`, `tag:example.com,2000:app/x`.  A tag in the standard namespace
   * always reaches here in the `!!x` spelling, whichever of 5.6's three
   * forms it was written in, so two nodes carrying the same tag compare
   * equal.  Tags outside that namespace are the URI, not expanded further -
   * which spelling to print is the caller's question. */
  const char * tag;

  /** SCALAR: the text as written, after escapes were decoded and block
   * scalars were folded and chomped - never NULL, and `""` for the empty
   * node.  ALIAS: the anchor name it refers to, without the `*`.  Empty for
   * every other event type.
   *
   * Always NUL-terminated, but the length is authoritative: a scalar may
   * contain a NUL. */
  const char * value;
  size_t value_len;   /**< Length of @ref value, excluding the terminator. */

  /** SCALAR only: how the scalar was written. */
  GTEXT_YAML_Scalar_Style scalar_style;

  /** SEQUENCE_START and MAPPING_START only: how the collection was written.
   *
   * `GTEXT_YAML_FLOW_STYLE_AUTO` for a node that was built through the DOM
   * API rather than parsed, which was never written in either style. */
  GTEXT_YAML_Flow_Style flow_style;

  /** DOCUMENT_START: `---` opened this document.  DOCUMENT_END: `...` closed
   * it.  False where the boundary was there but not written. */
  bool explicit_marker;
} GTEXT_YAML_Node_Event;

/**
 * @brief Callback invoked once per event.
 *
 * Returning anything but GTEXT_YAML_OK stops the walk and is returned from
 * the walk function unchanged.
 */
typedef GTEXT_YAML_Status (*GTEXT_YAML_Node_Event_Callback)(
  const GTEXT_YAML_Node_Event * event,
  void * user
);

/**
 * @brief Walk one document, without the surrounding stream events.
 *
 * Produces DOCUMENT_START, the document's node events, and DOCUMENT_END.  A
 * document with no root - `---` with nothing after it - has the empty node as
 * its root and so produces one SCALAR, which is what the document contains.
 *
 * @param doc Document to walk
 * @param cb Callback (must not be NULL)
 * @param user Passed to the callback unchanged
 * @return GTEXT_YAML_OK, GTEXT_YAML_E_INVALID for a NULL argument, or
 *         whatever the callback returned when it stopped the walk
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_document_walk(
  const GTEXT_YAML_Document * doc,
  GTEXT_YAML_Node_Event_Callback cb,
  void * user
);

/**
 * @brief Walk a whole stream of documents.
 *
 * Brackets @ref gtext_yaml_document_walk over each of @p docs with
 * STREAM_START and STREAM_END.  A stream with no documents in it is
 * STREAM_START followed by STREAM_END, which is a stream and not an error.
 *
 * @param docs Documents, as gtext_yaml_parse_all() returns them
 * @param count How many
 * @param cb Callback (must not be NULL)
 * @param user Passed to the callback unchanged
 * @return GTEXT_YAML_OK, GTEXT_YAML_E_INVALID for a NULL argument, or
 *         whatever the callback returned when it stopped the walk
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_stream_walk(
  GTEXT_YAML_Document * const * docs,
  size_t count,
  GTEXT_YAML_Node_Event_Callback cb,
  void * user
);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_YAML_YAML_EVENTS_H
