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
 * @brief Walking a parsed document as composed node events.
 */

#include <string.h>

#include <ghoti.io/text/yaml/yaml_core.h>
#include <ghoti.io/text/yaml/yaml_events.h>

#include "yaml_internal.h"

/* The empty node (7.2) has no text.  Scalar events point `value` here rather
   than at NULL so that a caller never has to check before reading it. */
static const char yaml_event_empty[] = "";


static GTEXT_YAML_Status emit(
	GTEXT_YAML_Node_Event *event,
	GTEXT_YAML_Node_Event_Callback cb,
	void *user
) {
	return cb(event, user);
}

/**
 * @brief The empty node, which has no node of its own to point at.
 *
 * A mapping pair with no value written after the ":" and a sequence entry
 * with nothing after the "-" both hold a NULL child.  That is a node all the
 * same - null, with no properties - and leaving it out would make the walk
 * disagree with the tree about how many entries the collection has.
 */
static GTEXT_YAML_Status walk_empty_node(
	GTEXT_YAML_Node_Event_Callback cb,
	void *user
) {
	GTEXT_YAML_Node_Event event;
	memset(&event, 0, sizeof(event));
	event.type = GTEXT_YAML_NODE_EVENT_SCALAR;
	event.value = yaml_event_empty;
	event.scalar_style = GTEXT_YAML_SCALAR_STYLE_PLAIN;
	return emit(&event, cb, user);
}

/* Open a collection: the START event, which is all a frame needs before its
   children are walked.  The END event is emit_collection_end(). */
static GTEXT_YAML_Status emit_collection_start(
	const GTEXT_YAML_Node *node,
	GTEXT_YAML_Node_Event_Callback cb,
	void *user
) {
	GTEXT_YAML_Node_Event event;
	const bool is_mapping = (node->type == GTEXT_YAML_MAPPING
		|| node->type == GTEXT_YAML_SET);

	memset(&event, 0, sizeof(event));
	event.type = is_mapping
		? GTEXT_YAML_NODE_EVENT_MAPPING_START
		: GTEXT_YAML_NODE_EVENT_SEQUENCE_START;
	event.node = node;
	event.anchor = is_mapping ? node->as.mapping.anchor : node->as.sequence.anchor;
	event.tag = is_mapping ? node->as.mapping.tag : node->as.sequence.tag;
	event.value = yaml_event_empty;
	event.flow_style = is_mapping
		? node->as.mapping.flow_style : node->as.sequence.flow_style;
	return emit(&event, cb, user);
}

static GTEXT_YAML_Status emit_collection_end(
	const GTEXT_YAML_Node *node,
	GTEXT_YAML_Node_Event_Callback cb,
	void *user
) {
	GTEXT_YAML_Node_Event event;
	const bool is_mapping = (node->type == GTEXT_YAML_MAPPING
		|| node->type == GTEXT_YAML_SET);

	memset(&event, 0, sizeof(event));
	event.type = is_mapping
		? GTEXT_YAML_NODE_EVENT_MAPPING_END
		: GTEXT_YAML_NODE_EVENT_SEQUENCE_END;
	event.node = node;
	event.value = yaml_event_empty;
	return emit(&event, cb, user);
}

static GTEXT_YAML_Status walk_scalar(
	const GTEXT_YAML_Node *node,
	GTEXT_YAML_Node_Event_Callback cb,
	void *user
) {
	const yaml_node_scalar *scalar = &node->as.scalar;
	GTEXT_YAML_Node_Event event;

	memset(&event, 0, sizeof(event));
	event.type = GTEXT_YAML_NODE_EVENT_SCALAR;
	event.node = node;
	event.anchor = scalar->anchor;
	event.tag = scalar->tag;
	event.value = scalar->value ? scalar->value : yaml_event_empty;
	event.value_len = scalar->value ? scalar->length : 0;
	event.scalar_style = scalar->scalar_style;
	return emit(&event, cb, user);
}

static GTEXT_YAML_Status walk_alias(
	const GTEXT_YAML_Node *node,
	GTEXT_YAML_Node_Event_Callback cb,
	void *user
) {
	const yaml_node_alias *alias = &node->as.alias;
	GTEXT_YAML_Node_Event event;

	memset(&event, 0, sizeof(event));
	event.type = GTEXT_YAML_NODE_EVENT_ALIAS;
	event.node = node;
	/* The target is deliberately not followed.  An alias may refer to a node
	   that contains it, so a walk that expanded them would not terminate. */
	event.value = alias->anchor_name ? alias->anchor_name : yaml_event_empty;
	event.value_len = event.value ? strlen(event.value) : 0;
	return emit(&event, cb, user);
}

/* Where the walk is in one collection: which child comes next, and - for a
   mapping - whether the pair's key has been emitted and its value is next. */
typedef struct {
	const GTEXT_YAML_Node *node;
	size_t i;
	bool on_value;
} yaml_walk_frame;

static bool walk_is_mapping(const GTEXT_YAML_Node *node) {
	return node->type == GTEXT_YAML_MAPPING || node->type == GTEXT_YAML_SET;
}

static bool walk_is_collection(const GTEXT_YAML_Node *node) {
	switch (node->type) {
		case GTEXT_YAML_SEQUENCE:
		case GTEXT_YAML_OMAP:
		case GTEXT_YAML_PAIRS:
		case GTEXT_YAML_MAPPING:
		case GTEXT_YAML_SET:
			return true;
		default:
			return false;
	}
}

/* Emit whatever @p node is, and say whether it opened a collection whose
   children still have to be walked.
 *
 * !!omap and !!pairs are sequences of single-pair mappings that carry a tag:
 * the tag is on the event and the shape is the sequence's own.  NULL, BOOL,
 * INT, FLOAT and STRING are all one scalar that the resolver typed, and the
 * text it was typed from is what the walk reports. */
static GTEXT_YAML_Status walk_enter(
	const GTEXT_YAML_Node *node,
	GTEXT_YAML_Node_Event_Callback cb,
	void *user,
	bool *out_opened
) {
	*out_opened = false;
	if (!node) return walk_empty_node(cb, user);
	if (walk_is_collection(node)) {
		*out_opened = true;
		return emit_collection_start(node, cb, user);
	}
	if (node->type == GTEXT_YAML_ALIAS) return walk_alias(node, cb, user);
	return walk_scalar(node, cb, user);
}

/* Walk @p root, emitting one event per node, with the walk's own stack on the
 * heap.
 *
 * This was three functions calling each other - walk_node, walk_sequence,
 * walk_mapping - and so a recursion over the whole document.  It is the sixth
 * walk over a DOM in this library to be converted and the worst of them: the
 * other five needed max_depth = SIZE_MAX to be asked for before they could be
 * taken past the end of the stack, and this one **ignored max_depth
 * altogether**, so a document built through the DOM API with every option at
 * its default went two hundred thousand levels deep and took the process with
 * it.
 *
 * It also outlived the sweep that found the others, because that sweep looked
 * for a function that calls its own name and this recursion goes round three.
 *
 * So both halves, the way gtext_yaml_node_clone() and write_node() have them:
 * the stack is on the heap, so depth costs memory rather than a frame, and
 * @p max_depth still bounds a *built* document - which is the half of the
 * library that can nest without limit, since max_depth was a parser limit and
 * said nothing about a document nobody parsed. */
static GTEXT_YAML_Status walk_node(
	const GTEXT_YAML_Node *root,
	GTEXT_YAML_Node_Event_Callback cb,
	void *user,
	size_t max_depth,
	const GTEXT_Allocator *alloc
) {
	yaml_walk_frame *stack = NULL;
	size_t count = 0, capacity = 0;
	GTEXT_YAML_Status status;
	bool opened = false;

	status = walk_enter(root, cb, user, &opened);
	if (status != GTEXT_YAML_OK || !opened) return status;

	#define YAML_WALK_PUSH(n)                                                  \
		do {                                                                   \
			if (max_depth > 0 && count >= max_depth) {                         \
				status = GTEXT_YAML_E_DEPTH;                                   \
				goto done;                                                     \
			}                                                                  \
			if (count == capacity) {                                           \
				size_t new_capacity = capacity == 0 ? 32 : capacity * 2;        \
				yaml_walk_frame *items = (yaml_walk_frame *)                    \
					gtext_allocator_realloc(alloc, stack,                       \
						new_capacity * sizeof(*items));                         \
				if (!items) { status = GTEXT_YAML_E_OOM; goto done; }           \
				stack = items;                                                 \
				capacity = new_capacity;                                       \
			}                                                                  \
			stack[count].node = (n);                                           \
			stack[count].i = 0;                                                \
			stack[count].on_value = false;                                     \
			count++;                                                           \
		} while (0)

	YAML_WALK_PUSH(root);

	while (count > 0) {
		yaml_walk_frame *f = &stack[count - 1];
		const GTEXT_YAML_Node *child = NULL;
		bool have_child = false;

		if (walk_is_mapping(f->node)) {
			if (f->i < f->node->as.mapping.count) {
				if (!f->on_value) {
					child = f->node->as.mapping.pairs[f->i].key;
					f->on_value = true;
				}
				else {
					child = f->node->as.mapping.pairs[f->i].value;
					f->on_value = false;
					f->i++;
				}
				have_child = true;
			}
		}
		else if (f->i < f->node->as.sequence.count) {
			child = f->node->as.sequence.children[f->i];
			f->i++;
			have_child = true;
		}

		if (!have_child) {
			const GTEXT_YAML_Node *done_node = f->node;
			count--;
			status = emit_collection_end(done_node, cb, user);
			if (status != GTEXT_YAML_OK) goto done;
			continue;
		}

		status = walk_enter(child, cb, user, &opened);
		if (status != GTEXT_YAML_OK) goto done;
		/* The push can move the array, so nothing may be read from @c f after
		   it; the loop re-derives the frame. */
		if (opened) YAML_WALK_PUSH(child);
	}

	status = GTEXT_YAML_OK;

done:
	#undef YAML_WALK_PUSH
	gtext_allocator_free(alloc, stack);
	return status;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_document_walk(
	const GTEXT_YAML_Document * doc,
	GTEXT_YAML_Node_Event_Callback cb,
	void * user
) {
	GTEXT_YAML_Node_Event event;
	GTEXT_YAML_Status status;

	if (!doc || !cb) return GTEXT_YAML_E_INVALID;

	memset(&event, 0, sizeof(event));
	event.type = GTEXT_YAML_NODE_EVENT_DOCUMENT_START;
	event.value = yaml_event_empty;
	event.explicit_marker = doc->explicit_start;
	status = emit(&event, cb, user);
	if (status != GTEXT_YAML_OK) return status;

	/* A document with no root is a document holding the empty node, not a
	   document holding nothing: walk_node() answers both the same way. */
	/* The document's own limit and allocator: a document carries the options
	   it was made with, and a walk is accounted to the same place it is. */
	status = walk_node(
		doc->root, cb, user, doc->options.max_depth, doc->ctx ? doc->ctx->alloc : NULL
	);
	if (status != GTEXT_YAML_OK) return status;

	memset(&event, 0, sizeof(event));
	event.type = GTEXT_YAML_NODE_EVENT_DOCUMENT_END;
	event.value = yaml_event_empty;
	event.explicit_marker = doc->explicit_end;
	return emit(&event, cb, user);
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_stream_walk(
	GTEXT_YAML_Document * const * docs,
	size_t count,
	GTEXT_YAML_Node_Event_Callback cb,
	void * user
) {
	GTEXT_YAML_Node_Event event;
	GTEXT_YAML_Status status;

	/* A count of zero is a stream with no documents in it, which is a stream;
	   docs may be NULL only in that case. */
	if (!cb || (!docs && count > 0)) return GTEXT_YAML_E_INVALID;

	memset(&event, 0, sizeof(event));
	event.type = GTEXT_YAML_NODE_EVENT_STREAM_START;
	event.value = yaml_event_empty;
	status = emit(&event, cb, user);
	if (status != GTEXT_YAML_OK) return status;

	for (size_t i = 0; i < count; i++) {
		status = gtext_yaml_document_walk(docs[i], cb, user);
		if (status != GTEXT_YAML_OK) return status;
	}

	memset(&event, 0, sizeof(event));
	event.type = GTEXT_YAML_NODE_EVENT_STREAM_END;
	event.value = yaml_event_empty;
	return emit(&event, cb, user);
}
