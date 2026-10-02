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
 * @file yaml_parser.c
 * @brief DOM parser - converts streaming events to DOM tree
 *
 * Implements gtext_yaml_parse() by using the streaming parser internally
 * and building a DOM tree from events. Uses a stack to track nesting.
 */

#define _POSIX_C_SOURCE 200809L  /* for strdup */

#include <ghoti.io/text/macros.h>
#include "yaml_internal.h"
#include <ghoti.io/text/yaml/yaml_stream.h>
#include <ghoti.io/text/json/json_dom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Saved temp state for a nesting level */
typedef struct {
	GTEXT_YAML_Node **items;
	size_t count;
	size_t capacity;
	char *anchor;  /* Anchor for this collection level (malloc'd, NULL if none) */
	char *tag;     /* Tag for this collection level (malloc'd, NULL if none) */
	size_t source_offset; /* Byte offset for collection start */
	int source_line;      /* 1-based line for collection start */
	int source_col;       /* 1-based column for collection start */
} saved_temp;

/* Anchor map entry */
typedef struct {
	char *name;              /* Anchor name (malloc'd) */
	GTEXT_YAML_Node *node;   /* Associated node */
} anchor_entry;

typedef struct {
	char *handle;
	char *prefix;
} tag_handle_entry;

/* Per-level flow flags.  PAIR marks a "[a: 1]" single-pair mapping, which has
 * no "}" and is closed by the "," or "]" that ends the entry.  ITEM_DONE says
 * a flow collection already holds a complete entry, so the next one needs a
 * "," in front of it. */
#define GTEXT_YAML_FLOW_PAIR      0x1u
#define GTEXT_YAML_FLOW_ITEM_DONE 0x2u
/* A block sequence's "-" has been seen and nothing has landed under it yet.
 * An entry with no node is the empty node, which is null (7.2, e-node): "-"
 * on its own is [null] and "- # comment" is an entry too. Both were being
 * dropped, so a sequence came back one entry short with no sign of it. */
#define GTEXT_YAML_BLOCK_ENTRY_OPEN 0x4u

/* Parser state for building DOM from events */
typedef struct {
	yaml_context *ctx;                  /* Context owns arena */
	GTEXT_YAML_Document *doc;           /* Document being built */
	GTEXT_YAML_Error *error;            /* Error output */
	
	/* Stack for tracking nesting (sequences/mappings in progress) */
	struct {
		GTEXT_YAML_Node **nodes;        /* Stack of nodes being built */
		int *states;                    /* State per level: 0=seq, 1=map_key, 2=map_value */
		int *indents;                   /* Indent level for block collections */
		bool *is_block;                 /* True if this level is block-style */
		unsigned char *flow_flags;      /* GTEXT_YAML_FLOW_* bits, per level */
		saved_temp *temps;              /* Saved temp state per level */
		size_t capacity;
		size_t depth;
	} stack;
	
	/* Temporary storage for building collections */
	struct {
		GTEXT_YAML_Node **items;        /* Child nodes */
		size_t count;
		size_t capacity;
	} temp;
	
	/* Anchor map for resolving aliases */
	struct {
		anchor_entry *entries;          /* Array of anchor entries */
		size_t count;
		size_t capacity;
	} anchors;
	
	/* List of alias nodes to resolve after parsing */
	struct {
		GTEXT_YAML_Node **nodes;        /* Array of alias nodes */
		/* The node each alias pointed at when it was read. An anchor may be
		 * redefined, and an alias refers to "the most recent preceding node
		 * having the same anchor" (3.2.2.2), so the binding has to be taken
		 * at the alias rather than from the anchor map once parsing has
		 * finished. NULL where the anchor was not yet known. */
		GTEXT_YAML_Node **targets;
		/* True where the anchor had been *written* but its node was not
		 * finished yet, which is how a collection refers to itself:
		 *
		 *     a: &a [*a]      &a is written before the "[", so it is a
		 *                     preceding anchor even though the sequence it
		 *                     names does not exist until the "]"
		 *
		 * Only these may be looked up again once parsing has finished. An
		 * alias whose anchor had not been written at all is a forward
		 * reference and an error (7.1), and resolving those late is what
		 * made "a: *x" over "b: &x 1" quietly succeed. */
		bool *deferred;
		size_t count;
		size_t capacity;
	} aliases;

	/* Tag handle map for %TAG directives */
	struct {
		tag_handle_entry *entries;
		size_t count;
		size_t capacity;
	} tag_handles;
	
	GTEXT_YAML_Node *root;              /* Root node once complete */
	bool failed;                        /* True if error occurred */
	size_t document_count;              /* Number of documents seen (0-based) */
	bool document_started;              /* True if inside a document */
	bool first_document_complete;       /* True when first document is done */
	
	/* Block collection detection state */
	bool in_block_sequence;             /* True if we're building a block sequence */
	bool in_block_mapping;              /* True if we're building a block mapping */
	bool expect_mapping_value;          /* True if next item is a mapping value after : */
	int last_event_line;                /* Last event line processed */
	int last_scalar_line;               /* Last scalar event line */
	int last_scalar_col;                /* Last scalar event column */
	size_t last_scalar_offset;          /* Last scalar event byte offset */
	int last_scalar_key_col;            /* Last scalar key start column */
	GTEXT_YAML_Node *last_scalar_node;  /* Last scalar node seen */
	/* True when the last scalar's tag was written on an earlier line than the
	 * scalar itself.  A tag applies to the node that follows it, and in block
	 * context that node may turn out to be the collection this scalar begins
	 * rather than the scalar: "!custom" then "a: 1" tags the mapping, while
	 * "!custom a: 1" tags the key.  The events are otherwise identical. */
	bool last_scalar_tag_own_line;
	bool last_scalar_anchor_own_line;
	/* A property the stream could not place: written before the one on the
	 * last scalar, and on an earlier line, so it belongs to the block
	 * collection that scalar opens rather than to the scalar.  Held from the
	 * scalar event until the collection is pushed, which is one event later
	 * at most.  Still held when anything else happens means no collection
	 * opened and both properties named one node, which is an error - suite
	 * case 4JVG.  See GTEXT_YAML_Event::outer_anchor. */
	char *outer_anchor;
	char *outer_tag;
	/* True when the property above was held from an ALIAS event rather than a
	 * second property.  An alias carries none of its own (7.1), so if no
	 * collection claims this one the fault is that it was written on an
	 * alias - not that one node was given two.  Only the message differs;
	 * both are refused. */
	bool outer_from_alias;
	/* A directive has been read and the "---" that must follow it has not
	 * arrived yet.  9.2: l-directive-document ::= l-directive+
	 * l-explicit-document, so a directive prologue with no marker after it
	 * is not a document. */
	bool directive_awaits_marker;
	bool last_scalar_in_root;           /* True if last scalar stored in root */
	bool last_scalar_in_temp;           /* True if last scalar stored in temp */
	size_t last_scalar_temp_depth;      /* Stack depth when scalar added to temp */
	/* The last node a ":" or a "?" claimed as a mapping key. A block
	 * mapping's trailing key is allowed to have no value - "a:" at the end
	 * of a document is {"a": null} - but a scalar that was never claimed at
	 * all is not a key, and was being turned into one. Compared by identity
	 * so a nested collection's key cannot be mistaken for its parent's. */
	const GTEXT_YAML_Node *claimed_key;
	bool seen_yaml_directive;           /* One %YAML per document (6.8.1) */
	bool explicit_key_pending;          /* True if '?' indicator seen and key is pending */
	bool explicit_key_active;           /* True if explicit key stored and awaiting ':' */
	int explicit_key_indent;            /* Indent column for explicit key */
	/* The line the "?" was written on, or 0 when no explicit key is open.
	 *
	 * "?" takes s-l+block-indented(n,block-out), which includes
	 * ns-l-compact-mapping (8.2.2), so the key may begin on the "?"'s own
	 * line: "? earth: blue" has {earth: blue} for its key.  That is the same
	 * event sequence as "? a" on one line and "c: 1" on the next, where the
	 * explicit key never got a value and "c" opens a new entry - and the two
	 * differ only in whether the "?" shares the line. */
	int explicit_key_line;
	size_t explicit_key_depth;          /* Stack depth for explicit key mapping */
	char *pending_leading_comment;      /* Pending leading comment (malloc'd) */
	GTEXT_YAML_Node *last_emitted_node; /* Last node created for inline comments */
	int last_emitted_line;              /* Line for last_emitted_node */
	/* Set by stack_push() when the nesting limit is reached, so its callers -
	   which all report an allocation failure - can say what actually
	   happened.  max_depth was being counted at the "[" and "{" of a *flow*
	   collection only, in the stream layer, and block structure never touched
	   it: "- " five thousand times parsed to a DOM five thousand deep, which
	   is exactly the input a limit exists for.  It surfaced because the
	   writer turned that block nesting into flow nesting, which *is* counted,
	   so the writer's own output was then refused. */
	bool depth_exceeded;
} parser_state;

/* Stack states */
#define STATE_SEQUENCE 0
#define STATE_MAPPING_KEY 1
#define STATE_MAPPING_VALUE 2

/**
 * @brief Initialize parser state.
 */
static bool parser_init(parser_state *p, yaml_context *ctx, GTEXT_YAML_Error *error) {
	memset(p, 0, sizeof(*p));
	p->ctx = ctx;
	p->error = error;
	
	/* Allocate initial stack capacity */
	p->stack.capacity = 32;
	p->stack.nodes = (GTEXT_YAML_Node **)gtext_allocator_malloc(p->ctx->alloc, p->stack.capacity * sizeof(GTEXT_YAML_Node *));
	p->stack.states = (int *)gtext_allocator_malloc(p->ctx->alloc, p->stack.capacity * sizeof(int));
	p->stack.indents = (int *)gtext_allocator_malloc(p->ctx->alloc, p->stack.capacity * sizeof(int));
	p->stack.is_block = (bool *)gtext_allocator_malloc(p->ctx->alloc, p->stack.capacity * sizeof(bool));
	p->stack.flow_flags = (unsigned char *)gtext_allocator_calloc(p->ctx->alloc, p->stack.capacity, 1);
	p->stack.temps = (saved_temp *)gtext_allocator_calloc(p->ctx->alloc, p->stack.capacity, sizeof(saved_temp));
	
	if (!p->stack.nodes || !p->stack.states || !p->stack.indents ||
		!p->stack.is_block || !p->stack.flow_flags || !p->stack.temps) {
		gtext_allocator_free(p->ctx->alloc, p->stack.nodes);
		gtext_allocator_free(p->ctx->alloc, p->stack.states);
		gtext_allocator_free(p->ctx->alloc, p->stack.indents);
		gtext_allocator_free(p->ctx->alloc, p->stack.is_block);
		gtext_allocator_free(p->ctx->alloc, p->stack.flow_flags);
		gtext_allocator_free(p->ctx->alloc, p->stack.temps);
		return false;
	}
	
	/* Allocate temporary storage for collection children */
	p->temp.capacity = 16;
	p->temp.items = (GTEXT_YAML_Node **)gtext_allocator_malloc(p->ctx->alloc, p->temp.capacity * sizeof(GTEXT_YAML_Node *));
	if (!p->temp.items) {
		gtext_allocator_free(p->ctx->alloc, p->stack.nodes);
		gtext_allocator_free(p->ctx->alloc, p->stack.states);
		gtext_allocator_free(p->ctx->alloc, p->stack.indents);
		gtext_allocator_free(p->ctx->alloc, p->stack.is_block);
		gtext_allocator_free(p->ctx->alloc, p->stack.flow_flags);
		gtext_allocator_free(p->ctx->alloc, p->stack.temps);
		return false;
	}
	
	/* Allocate anchor map */
	p->anchors.capacity = 16;
	p->anchors.entries = (anchor_entry *)gtext_allocator_calloc(p->ctx->alloc, p->anchors.capacity, sizeof(anchor_entry));
	if (!p->anchors.entries) {
		gtext_allocator_free(p->ctx->alloc, p->stack.nodes);
		gtext_allocator_free(p->ctx->alloc, p->stack.states);
		gtext_allocator_free(p->ctx->alloc, p->stack.indents);
		gtext_allocator_free(p->ctx->alloc, p->stack.is_block);
		gtext_allocator_free(p->ctx->alloc, p->stack.flow_flags);
		gtext_allocator_free(p->ctx->alloc, p->stack.temps);
		gtext_allocator_free(p->ctx->alloc, p->temp.items);
		return false;
	}
	
	/* Allocate alias list */
	p->aliases.capacity = 16;
	p->aliases.nodes = (GTEXT_YAML_Node **)gtext_allocator_malloc(p->ctx->alloc, p->aliases.capacity * sizeof(GTEXT_YAML_Node *));
	p->aliases.targets = (GTEXT_YAML_Node **)gtext_allocator_malloc(p->ctx->alloc, p->aliases.capacity * sizeof(GTEXT_YAML_Node *));
	p->aliases.deferred = (bool *)gtext_allocator_malloc(p->ctx->alloc, p->aliases.capacity * sizeof(bool));
	if (!p->aliases.nodes || !p->aliases.targets || !p->aliases.deferred) {
		gtext_allocator_free(p->ctx->alloc, p->aliases.nodes);
		gtext_allocator_free(p->ctx->alloc, p->aliases.targets);
		gtext_allocator_free(p->ctx->alloc, p->aliases.deferred);
		p->aliases.nodes = NULL;
		p->aliases.targets = NULL;
		p->aliases.deferred = NULL;
		gtext_allocator_free(p->ctx->alloc, p->stack.nodes);
		gtext_allocator_free(p->ctx->alloc, p->stack.states);
		gtext_allocator_free(p->ctx->alloc, p->stack.indents);
		gtext_allocator_free(p->ctx->alloc, p->stack.is_block);
		gtext_allocator_free(p->ctx->alloc, p->stack.flow_flags);
		gtext_allocator_free(p->ctx->alloc, p->stack.temps);
		gtext_allocator_free(p->ctx->alloc, p->temp.items);
		gtext_allocator_free(p->ctx->alloc, p->anchors.entries);
		return false;
	}

	/* Tag handles start empty */
	p->tag_handles.entries = NULL;
	p->tag_handles.count = 0;
	p->tag_handles.capacity = 0;

	p->last_event_line = -1;
	p->last_scalar_line = -1;
	p->last_scalar_col = -1;
	p->last_scalar_offset = 0;
	p->claimed_key = NULL;
	p->seen_yaml_directive = false;
	p->last_scalar_key_col = -1;
	p->last_scalar_node = NULL;
	p->last_scalar_tag_own_line = false;
	p->last_scalar_anchor_own_line = false;
	p->outer_anchor = NULL;
	p->outer_tag = NULL;
	p->outer_from_alias = false;
	p->directive_awaits_marker = false;
	p->pending_leading_comment = NULL;
	p->last_emitted_node = NULL;
	p->last_emitted_line = -1;
	p->last_scalar_in_root = false;
	p->last_scalar_in_temp = false;
	p->last_scalar_temp_depth = 0;
	p->explicit_key_pending = false;
	p->explicit_key_active = false;
	p->explicit_key_indent = -1;
	p->explicit_key_line = 0;
	p->explicit_key_depth = 0;
	
	return true;
}

/**
 * @brief Free parser state.
 */
static void parser_free(parser_state *p) {
	/* Read once, before anything is released: every free below goes through
	   the allocator the context named, and the context outlives this call. */
	const GTEXT_Allocator *alloc = p->ctx ? p->ctx->alloc : NULL;

	/* Free saved temp arrays and metadata */
	for (size_t i = 0; i < p->stack.depth; i++) {
		gtext_allocator_free(alloc, p->stack.temps[i].items);
		gtext_allocator_free(alloc, p->stack.temps[i].anchor);
		gtext_allocator_free(alloc, p->stack.temps[i].tag);
	}
	/* Also check capacity range for any lingering allocated metadata */
	for (size_t i = p->stack.depth; i < p->stack.capacity; i++) {
		gtext_allocator_free(alloc, p->stack.temps[i].anchor);
		gtext_allocator_free(alloc, p->stack.temps[i].tag);
	}
	gtext_allocator_free(alloc, p->stack.nodes);
	gtext_allocator_free(alloc, p->stack.states);
	gtext_allocator_free(alloc, p->stack.indents);
	gtext_allocator_free(alloc, p->stack.is_block);
	gtext_allocator_free(alloc, p->stack.flow_flags);
	gtext_allocator_free(alloc, p->stack.temps);
	gtext_allocator_free(alloc, p->temp.items);
	
	/* Free anchor map */
	for (size_t i = 0; i < p->anchors.count; i++) {
		gtext_allocator_free(alloc, p->anchors.entries[i].name);
	}
	gtext_allocator_free(alloc, p->anchors.entries);
	
	/* Free alias list */
	gtext_allocator_free(alloc, p->aliases.nodes);
	gtext_allocator_free(alloc, p->aliases.targets);
	gtext_allocator_free(alloc, p->aliases.deferred);

	gtext_allocator_free(alloc, p->pending_leading_comment);
	gtext_allocator_free(alloc, p->outer_anchor);
	gtext_allocator_free(alloc, p->outer_tag);

	/* Free tag handles */
	for (size_t i = 0; i < p->tag_handles.count; i++) {
		gtext_allocator_free(alloc, p->tag_handles.entries[i].handle);
		gtext_allocator_free(alloc, p->tag_handles.entries[i].prefix);
	}
	gtext_allocator_free(alloc, p->tag_handles.entries);
}

/* Forward declarations */
static GTEXT_YAML_Node *lookup_anchor(parser_state *p, const char *name);

static const char *parser_arena_strdup(
	yaml_context *ctx,
	const char *str,
	size_t len
) {
	if (!ctx || !str) return NULL;
	char *copy = (char *)yaml_context_alloc(ctx, len + 1, 1);
	if (!copy) return NULL;
	memcpy(copy, str, len);
	copy[len] = '\0';
	return copy;
}

static void parser_attach_leading_comment(parser_state *p, GTEXT_YAML_Node *node) {
	if (!p || !node || !p->pending_leading_comment || !p->ctx) return;
	const char *stored = parser_arena_strdup(
		p->ctx,
		p->pending_leading_comment,
		strlen(p->pending_leading_comment)
	);
	if (!stored) return;
	switch (node->type) {
		case GTEXT_YAML_STRING:
		case GTEXT_YAML_BOOL:
		case GTEXT_YAML_INT:
		case GTEXT_YAML_FLOAT:
		case GTEXT_YAML_NULL:
			node->as.scalar.leading_comment = stored;
			break;
		case GTEXT_YAML_SEQUENCE:
		case GTEXT_YAML_OMAP:
		case GTEXT_YAML_PAIRS:
			node->as.sequence.leading_comment = stored;
			break;
		case GTEXT_YAML_MAPPING:
		case GTEXT_YAML_SET:
			node->as.mapping.leading_comment = stored;
			break;
		case GTEXT_YAML_ALIAS:
			node->as.alias.leading_comment = stored;
			break;
		default:
			break;
	}
	gtext_allocator_free(p->ctx->alloc, p->pending_leading_comment);
	p->pending_leading_comment = NULL;
}

static void parser_attach_inline_comment(
	parser_state *p,
	GTEXT_YAML_Node *node,
	const char *comment
) {
	if (!p || !node || !comment || !p->ctx) return;
	const char *stored = parser_arena_strdup(p->ctx, comment, strlen(comment));
	if (!stored) return;
	switch (node->type) {
		case GTEXT_YAML_STRING:
		case GTEXT_YAML_BOOL:
		case GTEXT_YAML_INT:
		case GTEXT_YAML_FLOAT:
		case GTEXT_YAML_NULL:
			node->as.scalar.inline_comment = stored;
			break;
		case GTEXT_YAML_SEQUENCE:
		case GTEXT_YAML_OMAP:
		case GTEXT_YAML_PAIRS:
			node->as.sequence.inline_comment = stored;
			break;
		case GTEXT_YAML_MAPPING:
		case GTEXT_YAML_SET:
			node->as.mapping.inline_comment = stored;
			break;
		case GTEXT_YAML_ALIAS:
			node->as.alias.inline_comment = stored;
			break;
		default:
			break;
	}
}

static bool json_fastpath_candidate(const char *input, size_t length) {
	size_t i = 0;
	size_t depth = 0;
	bool in_string = false;
	bool escape = false;
	struct {
		int type;
		bool expect_key;
	} stack[64];
	const int ctx_object = 1;
	const int ctx_array = 2;

	if (!input || length == 0) return false;

	if (length >= 4) {
		unsigned char b0 = (unsigned char)input[0];
		unsigned char b1 = (unsigned char)input[1];
		unsigned char b2 = (unsigned char)input[2];
		unsigned char b3 = (unsigned char)input[3];
		if ((b0 == 0x00 && b1 == 0x00 && b2 == 0xFE && b3 == 0xFF) ||
			(b0 == 0xFF && b1 == 0xFE && b2 == 0x00 && b3 == 0x00)) {
			return false;
		}
	}

	if (length >= 2) {
		unsigned char b0 = (unsigned char)input[0];
		unsigned char b1 = (unsigned char)input[1];
		if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) {
			return false;
		}
	}

	if (length >= 3 &&
		(unsigned char)input[0] == 0xEF &&
		(unsigned char)input[1] == 0xBB &&
		(unsigned char)input[2] == 0xBF) {
		i = 3;
	}

	while (i < length) {
		char ch = input[i];
		if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
			break;
		}
		i++;
	}

	if (i >= length) return false;
	if (input[i] != '{' && input[i] != '[') return false;

	for (size_t j = i; j < length; j++) {
		char ch = input[j];
		if (in_string) {
			if (escape) {
				escape = false;
				continue;
			}
			if (ch == '\\') {
				escape = true;
				continue;
			}
			if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			if (depth > 0 && stack[depth - 1].type == ctx_object &&
				stack[depth - 1].expect_key) {
				stack[depth - 1].expect_key = false;
			}
			continue;
		}

		if (ch == '#') return false;
		if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;

		if (ch == '{') {
			if (depth >= sizeof(stack) / sizeof(stack[0])) return false;
			stack[depth].type = ctx_object;
			stack[depth].expect_key = true;
			depth++;
			continue;
		}

		if (ch == '[') {
			if (depth >= sizeof(stack) / sizeof(stack[0])) return false;
			stack[depth].type = ctx_array;
			stack[depth].expect_key = false;
			depth++;
			continue;
		}

		if (ch == '}') {
			if (depth == 0 || stack[depth - 1].type != ctx_object) return false;
			depth--;
			continue;
		}

		if (ch == ']') {
			if (depth == 0 || stack[depth - 1].type != ctx_array) return false;
			depth--;
			continue;
		}

		if (ch == ',') {
			if (depth > 0 && stack[depth - 1].type == ctx_object) {
				stack[depth - 1].expect_key = true;
			}
			continue;
		}

		if (ch == ':') {
			if (depth > 0 && stack[depth - 1].type == ctx_object) {
				stack[depth - 1].expect_key = false;
			}
			continue;
		}

		if (depth > 0 && stack[depth - 1].type == ctx_object &&
			stack[depth - 1].expect_key) {
			return false;
		}
	}

	return true;
}

static GTEXT_JSON_Dupkey_Mode json_dupkey_mode(GTEXT_YAML_Dupkey_Mode mode) {
	switch (mode) {
		case GTEXT_YAML_DUPKEY_FIRST_WINS:
			return GTEXT_JSON_DUPKEY_FIRST_WINS;
		case GTEXT_YAML_DUPKEY_LAST_WINS:
			return GTEXT_JSON_DUPKEY_LAST_WINS;
		case GTEXT_YAML_DUPKEY_KEEP_ALL:
			/* Unreachable while enable_json_fast_path is forced off for this
			   mode below, and here so that the switch names every value: the
			   JSON DOM has no way to hold two pairs with the same key, so
			   there is no honest translation of KEEP_ALL. */
		case GTEXT_YAML_DUPKEY_ERROR:
		default:
			return GTEXT_JSON_DUPKEY_ERROR;
	}
}

static GTEXT_JSON_Parse_Options json_parse_options_from_yaml(
	const GTEXT_YAML_Parse_Options *opts
) {
	GTEXT_JSON_Parse_Options json_opts = gtext_json_parse_options_default();

	if (!opts) return json_opts;

	json_opts.dupkeys = json_dupkey_mode(opts->dupkeys);
	json_opts.validate_utf8 = opts->validate_utf8;
	if (opts->max_depth > 0) json_opts.max_depth = opts->max_depth;
	if (opts->max_total_bytes > 0) json_opts.max_total_bytes = opts->max_total_bytes;

	return json_opts;
}

static void map_json_error(const GTEXT_JSON_Error *json_err, GTEXT_YAML_Error *yaml_err) {
	if (!json_err || !yaml_err) return;

	switch (json_err->code) {
		case GTEXT_JSON_E_OOM:
			yaml_err->code = GTEXT_YAML_E_OOM;
			break;
		case GTEXT_JSON_E_LIMIT:
			yaml_err->code = GTEXT_YAML_E_LIMIT;
			break;
		case GTEXT_JSON_E_DEPTH:
			yaml_err->code = GTEXT_YAML_E_DEPTH;
			break;
		case GTEXT_JSON_E_INCOMPLETE:
			yaml_err->code = GTEXT_YAML_E_INCOMPLETE;
			break;
		case GTEXT_JSON_E_BAD_TOKEN:
			yaml_err->code = GTEXT_YAML_E_BAD_TOKEN;
			break;
		case GTEXT_JSON_E_BAD_ESCAPE:
			yaml_err->code = GTEXT_YAML_E_BAD_ESCAPE;
			break;
		default:
			yaml_err->code = GTEXT_YAML_E_INVALID;
			break;
	}

	yaml_err->message = json_err->message;
	yaml_err->offset = json_err->offset;
	yaml_err->line = json_err->line;
	yaml_err->col = json_err->col;
	yaml_err->context_snippet = NULL;
	yaml_err->context_snippet_len = 0;
	yaml_err->caret_offset = 0;
	yaml_err->expected_token = NULL;
	yaml_err->actual_token = NULL;
}

/* A JSON string is a *string*, and the resolver has to be told so.
 *
 * It resolves a scalar by its contents only when the scalar was written
 * plain - "only a plain scalar is resolved by its contents", 10.3.2, which is
 * the whole point of quoting - and a node built here carries no style, so
 * every JSON string was being resolved as though it had been written bare:
 * ["0x10"] came back as [16], [""] as [null], and the key of {"": ""} as the
 * string "null".
 *
 * The general parser was taught this rule some time ago. This path is a
 * second implementation that never heard it, and nothing measured it: the
 * conformance runner asks for GTEXT_YAML_DUPKEY_KEEP_ALL, which is the one
 * setting that turns the fast path off, so all 395 cases have only ever gone
 * the other way. */
static GTEXT_YAML_Node *json_string_node(
	yaml_context *ctx,
	const char *value,
	size_t length
) {
	GTEXT_YAML_Node *node = yaml_node_new_scalar(ctx, value, length, NULL, NULL);
	if (node) {
		node->as.scalar.scalar_style = GTEXT_YAML_SCALAR_STYLE_DOUBLE_QUOTED;
	}
	return node;
}

static GTEXT_YAML_Node *json_to_yaml_node(
	yaml_context *ctx,
	const GTEXT_JSON_Value *json,
	GTEXT_YAML_Error *error
) {
	GTEXT_JSON_Type type = GTEXT_JSON_NULL;
	GTEXT_YAML_Node *node = NULL;

	if (!json) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Missing JSON value";
		}
		return NULL;
	}

	type = gtext_json_typeof(json);

	switch (type) {
		case GTEXT_JSON_NULL:
			return yaml_node_new_scalar(ctx, "null", 4, NULL, NULL);
		case GTEXT_JSON_BOOL: {
			bool value = false;
			if (gtext_json_get_bool(json, &value) != GTEXT_JSON_OK) {
				if (error) {
					error->code = GTEXT_YAML_E_INVALID;
					error->message = "Invalid JSON boolean value";
				}
				return NULL;
			}
			return yaml_node_new_scalar(ctx, value ? "true" : "false", value ? 4 : 5, NULL, NULL);
		}
		case GTEXT_JSON_NUMBER: {
			const char *lexeme = NULL;
			size_t lexeme_len = 0;
			if (gtext_json_get_number_lexeme(json, &lexeme, &lexeme_len) != GTEXT_JSON_OK || !lexeme) {
				if (error) {
					error->code = GTEXT_YAML_E_INVALID;
					error->message = "Invalid JSON number value";
				}
				return NULL;
			}
			return yaml_node_new_scalar(ctx, lexeme, lexeme_len, NULL, NULL);
		}
		case GTEXT_JSON_STRING: {
			const char *value = NULL;
			size_t value_len = 0;
			if (gtext_json_get_string(json, &value, &value_len) != GTEXT_JSON_OK || !value) {
				if (error) {
					error->code = GTEXT_YAML_E_INVALID;
					error->message = "Invalid JSON string value";
				}
				return NULL;
			}
			return json_string_node(ctx, value, value_len);
		}
		case GTEXT_JSON_ARRAY: {
			size_t count = gtext_json_array_size(json);
			node = yaml_node_new_sequence(ctx, count, NULL, NULL);
			if (!node) return NULL;
			/* JSON writes its collections the way YAML writes flow ones, and
			   this path is only taken for input that is JSON. */
			node->as.sequence.flow_style = GTEXT_YAML_FLOW_STYLE_FLOW;
			node->as.sequence.count = count;
			for (size_t i = 0; i < count; i++) {
				const GTEXT_JSON_Value *child = gtext_json_array_get(json, i);
				node->as.sequence.children[i] = json_to_yaml_node(ctx, child, error);
				if (!node->as.sequence.children[i]) return NULL;
			}
			return node;
		}
		case GTEXT_JSON_OBJECT: {
			size_t count = gtext_json_object_size(json);
			node = yaml_node_new_mapping(ctx, count, NULL, NULL);
			if (!node) return NULL;
			node->as.mapping.flow_style = GTEXT_YAML_FLOW_STYLE_FLOW;
			node->as.mapping.count = count;
			for (size_t i = 0; i < count; i++) {
				size_t key_len = 0;
				const char *key = gtext_json_object_key(json, i, &key_len);
				const GTEXT_JSON_Value *value = gtext_json_object_value(json, i);
				if (!key || !value) {
					if (error) {
						error->code = GTEXT_YAML_E_INVALID;
						error->message = "Invalid JSON object member";
					}
					return NULL;
				}
				node->as.mapping.pairs[i].key = json_string_node(ctx, key, key_len);
				if (!node->as.mapping.pairs[i].key) return NULL;
				node->as.mapping.pairs[i].value = json_to_yaml_node(ctx, value, error);
				if (!node->as.mapping.pairs[i].value) return NULL;
				node->as.mapping.pairs[i].key_tag = NULL;
				node->as.mapping.pairs[i].value_tag = NULL;
			}
			return node;
		}
		default:
			break;
	}

	if (error) {
		error->code = GTEXT_YAML_E_INVALID;
		error->message = "Unsupported JSON value type";
	}
	return NULL;
}

static GTEXT_YAML_Document *yaml_parse_json_document_internal(
	const char *input,
	size_t length,
	const GTEXT_YAML_Parse_Options *options,
	GTEXT_YAML_Error *error,
	bool report_errors
) {
	GTEXT_YAML_Parse_Options effective_opts =
		gtext_yaml_parse_options_effective(options);
	const GTEXT_YAML_Parse_Options *opts = &effective_opts;
	GTEXT_JSON_Parse_Options json_opts = json_parse_options_from_yaml(opts);
	GTEXT_JSON_Error json_err = {0};
	GTEXT_JSON_Value *json_root = gtext_json_parse(input, length, &json_opts, &json_err);
	GTEXT_YAML_Document *doc = NULL;
	yaml_context *ctx = NULL;
	GTEXT_YAML_Node *root = NULL;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;

	if (!json_root) {
		if (report_errors && error) {
			map_json_error(&json_err, error);
		}
		gtext_json_error_free(&json_err);
		return NULL;
	}
	gtext_json_error_free(&json_err);

	ctx = yaml_context_new(opts->allocator);
	if (!ctx) {
		gtext_json_free(json_root);
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory creating context";
		}
		return NULL;
	}

	doc = (GTEXT_YAML_Document *)yaml_context_alloc(ctx, sizeof(GTEXT_YAML_Document), 8);
	if (!doc) {
		yaml_context_free(ctx);
		gtext_json_free(json_root);
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory creating document";
		}
		return NULL;
	}

	memset(doc, 0, sizeof(*doc));
	doc->ctx = ctx;
	doc->options = *opts;
	doc->document_index = 0;

	root = json_to_yaml_node(ctx, json_root, error);
	gtext_json_free(json_root);
	if (!root) {
		yaml_context_free(ctx);
		return NULL;
	}

	doc->root = root;

	status = yaml_resolve_document(doc, error);
	if (status != GTEXT_YAML_OK) {
		yaml_context_free(ctx);
		return NULL;
	}

	/* After yaml_resolve_document(), not before: a merge key expands into a
	   mapping node that resolution creates (yaml_resolve.c), so a count taken
	   above misses it.  The context holds one count per document - every path
	   that builds a document gives it a context of its own - so this is that
	   document's own total and not a running one. */
	doc->node_count = ctx->node_count;

	return doc;
}

/**
 * @brief Register an anchor name with its node.
 */
static GTEXT_YAML_Status register_anchor(parser_state *p, const char *name, GTEXT_YAML_Node *node) {
	if (!name || !node) return GTEXT_YAML_OK;  /* No anchor to register */

	/* An anchor may be redefined, and the most recent definition is the one
	 * an alias resolves to (3.2.2.2: "the alias node refers to the most
	 * recent preceding node having the same anchor"). This refused the
	 * second definition outright, which made spec example 7.1 unparseable.
	 * Overwrite the entry so later aliases see the new node and earlier ones,
	 * already resolved, keep the old. */
	for (size_t i = 0; i < p->anchors.count; i++) {
		if (p->anchors.entries[i].name
				&& strcmp(p->anchors.entries[i].name, name) == 0) {
			p->anchors.entries[i].node = node;
			return GTEXT_YAML_OK;
		}
	}

	/* Check if we need to grow the anchor map */
	if (p->anchors.count >= p->anchors.capacity) {
		size_t new_cap = p->anchors.capacity * 2;
		anchor_entry *new_entries = (anchor_entry *)gtext_allocator_realloc(p->ctx->alloc, 
			p->anchors.entries, new_cap * sizeof(anchor_entry)
		);
		if (!new_entries) {
			p->failed = true;
			if (p->error) {
				p->error->code = GTEXT_YAML_E_OOM;
				p->error->message = "Out of memory registering anchor";
			}
			return GTEXT_YAML_E_OOM;
		}
		memset(new_entries + p->anchors.capacity, 0, (new_cap - p->anchors.capacity) * sizeof(anchor_entry));
		p->anchors.entries = new_entries;
		p->anchors.capacity = new_cap;
	}

	/* Add the anchor */
	p->anchors.entries[p->anchors.count].name = gtext_yaml_strdup(name, p->ctx->alloc);
	p->anchors.entries[p->anchors.count].node = node;
	if (!p->anchors.entries[p->anchors.count].name) {
		p->failed = true;
		if (p->error) {
			p->error->code = GTEXT_YAML_E_OOM;
			p->error->message = "Out of memory registering anchor";
		}
		return GTEXT_YAML_E_OOM;
	}
	p->anchors.count++;

	return GTEXT_YAML_OK;
}

/**
 * @brief Take back an anchor a scalar registered on a collection's behalf.
 *
 * An own-line anchor reaches the parser on the collection's first scalar -
 * its first key, or its first entry - because a block collection has no
 * opening token to carry it. That scalar registers it before
 * adopt_own_line_anchor() gets to look at it, and the registration says the
 * anchor names the scalar, which it never did.
 *
 * Leaving the entry in place is not merely untidy: it is what an alias
 * *inside* the collection resolves against. "&O\nk: v\nj: *O" came back
 * with *O bound to the string "k" rather than to the mapping, against both
 * PyYAML and js-yaml. The equivalent flow document, "&O [ 1, *O ]", was
 * always right, because there the anchor arrives with the "[" and no scalar
 * ever claims it.
 *
 * The entry keeps its name and gives up its node, which is exactly the state
 * a collection's anchor is in between its opening and its closing: written,
 * but with nothing yet to point at. lookup_anchor() then misses,
 * anchor_open_on_stack() finds the name on the level being built, and the
 * alias is deferred to the end of the parse - the same path the flow case
 * has always taken. register_anchor() fills the entry back in when the
 * collection exists.
 *
 * Only an entry still pointing at this scalar is cleared. A name may have
 * been anchored to something else earlier in the document, and an alias
 * written between that definition and this one has already bound to it.
 *
 * Of the two places adopt_own_line_anchor() moves an anchor off a scalar,
 * only one has a document behind it. The other is the handover into
 * p->outer_anchor, and instrumenting it across the suite and the conformance
 * corpus reaches it exactly once - from "top: &n" over "  &m" over "  - 1",
 * which is refused as a node with more than one anchor whatever this
 * function does. It is called there because the rule is the same on both
 * paths, not because an accepted document turns on it.
 */
static void unregister_anchor(
		parser_state *p, const char *name, const GTEXT_YAML_Node *node) {
	if (!p || !name || !node) return;
	for (size_t i = 0; i < p->anchors.count; i++) {
		if (p->anchors.entries[i].name
				&& strcmp(p->anchors.entries[i].name, name) == 0) {
			if (p->anchors.entries[i].node == node) {
				p->anchors.entries[i].node = NULL;
			}
			return;
		}
	}
}

/**
 * @brief Track an alias node for later resolution.
 */
static bool track_alias(
	parser_state *p,
	GTEXT_YAML_Node *alias_node,
	GTEXT_YAML_Node *target,
	bool deferred
) {
	if (!alias_node) return true;
	
	/* Check if we need to grow the alias list */
	if (p->aliases.count >= p->aliases.capacity) {
		size_t new_cap = p->aliases.capacity * 2;
		GTEXT_YAML_Node **new_nodes = (GTEXT_YAML_Node **)gtext_allocator_realloc(p->ctx->alloc, 
			p->aliases.nodes, new_cap * sizeof(GTEXT_YAML_Node *)
		);
		if (!new_nodes) return false;
		p->aliases.nodes = new_nodes;
		GTEXT_YAML_Node **new_targets = (GTEXT_YAML_Node **)gtext_allocator_realloc(p->ctx->alloc, 
			p->aliases.targets, new_cap * sizeof(GTEXT_YAML_Node *)
		);
		if (!new_targets) return false;
		p->aliases.targets = new_targets;
		bool *new_deferred = (bool *)gtext_allocator_realloc(p->ctx->alloc, 
			p->aliases.deferred, new_cap * sizeof(bool)
		);
		if (!new_deferred) return false;
		p->aliases.deferred = new_deferred;
		p->aliases.capacity = new_cap;
	}
	
	p->aliases.targets[p->aliases.count] = target;
	p->aliases.deferred[p->aliases.count] = deferred;
	p->aliases.nodes[p->aliases.count++] = alias_node;
	return true;
}

/**
 * @brief Has this anchor been written on a collection still being built?
 *
 * A collection's anchor is recorded on its stack level when the collection
 * opens and only reaches the anchor map when the node exists, at the "]" or
 * the dedent that closes it. Between the two an alias inside the collection
 * can legitimately name it - that is what a recursive document is - and
 * lookup_anchor() cannot see it yet.
 */
static bool anchor_open_on_stack(const parser_state *p, const char *name) {
	if (!p || !name) return false;
	for (size_t i = 0; i < p->stack.depth; i++) {
		const char *anchor = p->stack.temps[i].anchor;
		if (anchor && strcmp(anchor, name) == 0) return true;
	}
	return false;
}

/**
 * @brief Look up an anchor by name.
 */
static GTEXT_YAML_Node *lookup_anchor(parser_state *p, const char *name) {
	if (!name) return NULL;
	
	for (size_t i = 0; i < p->anchors.count; i++) {
		if (strcmp(p->anchors.entries[i].name, name) == 0) {
			return p->anchors.entries[i].node;
		}
	}
	
	return NULL;
}

static const char *context_strdup(yaml_context *ctx, const char *value) {
	if (!ctx || !value) return NULL;
	size_t len = strlen(value);
	char *copy = (char *)yaml_context_alloc(ctx, len + 1, 1);
	if (!copy) return NULL;
	memcpy(copy, value, len);
	copy[len] = '\0';
	return copy;
}

static const char *tag_handle_lookup(const parser_state *p, const char *handle) {
	if (!p || !handle) return NULL;
	for (size_t i = 0; i < p->tag_handles.count; i++) {
		if (strcmp(p->tag_handles.entries[i].handle, handle) == 0) {
			return p->tag_handles.entries[i].prefix;
		}
	}
	return NULL;
}

static bool tag_handle_add(parser_state *p, const char *handle, const char *prefix) {
	if (!p || !handle || !prefix) return false;

	for (size_t i = 0; i < p->tag_handles.count; i++) {
		if (strcmp(p->tag_handles.entries[i].handle, handle) == 0) {
			char *new_prefix = gtext_yaml_strdup(prefix, p->ctx->alloc);
			if (!new_prefix) return false;
			gtext_allocator_free(p->ctx->alloc, p->tag_handles.entries[i].prefix);
			p->tag_handles.entries[i].prefix = new_prefix;
			return true;
		}
	}

	if (p->tag_handles.count >= p->tag_handles.capacity) {
		size_t new_cap = p->tag_handles.capacity == 0 ? 8 : p->tag_handles.capacity * 2;
		tag_handle_entry *new_entries = (tag_handle_entry *)gtext_allocator_realloc(p->ctx->alloc, 
			p->tag_handles.entries, new_cap * sizeof(tag_handle_entry)
		);
		if (!new_entries) return false;
		p->tag_handles.entries = new_entries;
		p->tag_handles.capacity = new_cap;
	}

	p->tag_handles.entries[p->tag_handles.count].handle = gtext_yaml_strdup(handle, p->ctx->alloc);
	p->tag_handles.entries[p->tag_handles.count].prefix = gtext_yaml_strdup(prefix, p->ctx->alloc);
	if (!p->tag_handles.entries[p->tag_handles.count].handle ||
		!p->tag_handles.entries[p->tag_handles.count].prefix) {
		gtext_allocator_free(p->ctx->alloc, p->tag_handles.entries[p->tag_handles.count].handle);
		gtext_allocator_free(p->ctx->alloc, p->tag_handles.entries[p->tag_handles.count].prefix);
		return false;
	}
	p->tag_handles.count++;
	return true;
}

/**
 * @brief Resolve all alias nodes.
 */
static GTEXT_YAML_Status resolve_aliases(parser_state *p) {
	if (!p || !p->doc) return GTEXT_YAML_E_INVALID;
	size_t max_aliases = p->doc->options.max_alias_expansion;
	size_t alias_count = 0;

	for (size_t i = 0; i < p->aliases.count; i++) {
		GTEXT_YAML_Node *alias = p->aliases.nodes[i];
		if (alias->type != GTEXT_YAML_ALIAS) continue;  /* Shouldn't happen */

		if (max_aliases > 0 && alias_count + 1 > max_aliases) {
			p->failed = true;
			if (p->error) {
				p->error->code = GTEXT_YAML_E_LIMIT;
				p->error->message = "Alias expansion limit exceeded";
			}
			return GTEXT_YAML_E_LIMIT;
		}
		alias_count++;
		
		/* Bound when the alias was written, and only then.  An alias refers
		 * to the most recent *preceding* node carrying the anchor (7.1), so
		 * a name that was not yet anchored is an error even if the document
		 * goes on to define it.  Looking it up again here - after the whole
		 * document is parsed, when every anchor exists - resolved forward
		 * references silently: "a: *x" over "b: &x 1" came back as
		 * {"a": 1, "b": 1}. Both PyYAML and js-yaml refuse it. */
		GTEXT_YAML_Node *target = p->aliases.targets[i];
		if (!target && p->aliases.deferred[i]) {
			target = lookup_anchor(p, alias->as.alias.anchor_name);
		}

		if (!target) {
			/* Unknown anchor */
			p->failed = true;
			if (p->error) {
				p->error->code = GTEXT_YAML_E_INVALID;
				p->error->message = "Unknown anchor referenced by alias";
			}
			return GTEXT_YAML_E_INVALID;
		}
		
		alias->as.alias.target = target;
	}
	
	return GTEXT_YAML_OK;
}

static bool finalize_tag_handles(parser_state *p, GTEXT_YAML_Document *doc) {
	if (!p || !doc) return false;
	if (p->tag_handles.count == 0) return true;

	yaml_tag_handle *handles = (yaml_tag_handle *)yaml_context_alloc(
		doc->ctx, sizeof(yaml_tag_handle) * p->tag_handles.count, 8
	);
	if (!handles) return false;

	for (size_t i = 0; i < p->tag_handles.count; i++) {
		handles[i].handle = context_strdup(doc->ctx, p->tag_handles.entries[i].handle);
		handles[i].prefix = context_strdup(doc->ctx, p->tag_handles.entries[i].prefix);
		if (!handles[i].handle || !handles[i].prefix) return false;
	}

	doc->tag_handles = handles;
	doc->tag_handle_count = p->tag_handles.count;
	return true;
}

/**
 * @brief Push a node onto the stack (for tracking nesting).
 * Saves the current temp state and clears temp for the new level.
 */
/* What a failed stack_push() means.  Every caller used to report an
   allocation failure, which is right for the one cause it used to have and
   wrong for the limit. */
static GTEXT_YAML_Status stack_push_failed(
	parser_state *p, const char *oom_message) {
	p->failed = true;
	if (p->depth_exceeded) {
		if (p->error) {
			p->error->code = GTEXT_YAML_E_DEPTH;
			p->error->message = "Maximum nesting depth exceeded";
		}
		return GTEXT_YAML_E_DEPTH;
	}
	if (p->error) {
		p->error->code = GTEXT_YAML_E_OOM;
		p->error->message = oom_message;
	}
	return GTEXT_YAML_E_OOM;
}

static bool stack_push(
	parser_state *p,
	GTEXT_YAML_Node *node,
	int state,
	const char *anchor,
	const char *tag,
	int indent,
	bool is_block,
	size_t source_offset,
	int source_line,
	int source_col
) {
	const size_t max_depth = p->doc ? p->doc->options.max_depth : 0;
	if (max_depth > 0 && p->stack.depth >= max_depth) {
		p->depth_exceeded = true;
		return false;
	}
	if (p->stack.depth >= p->stack.capacity) {
		/* Grow stack */
		size_t new_cap = p->stack.capacity * 2;
		GTEXT_YAML_Node **new_nodes = (GTEXT_YAML_Node **)gtext_allocator_realloc(p->ctx->alloc, 
			p->stack.nodes, new_cap * sizeof(GTEXT_YAML_Node *)
		);
		int *new_states = (int *)gtext_allocator_realloc(p->ctx->alloc, p->stack.states, new_cap * sizeof(int));
		int *new_indents = (int *)gtext_allocator_realloc(p->ctx->alloc, p->stack.indents, new_cap * sizeof(int));
		bool *new_is_block = (bool *)gtext_allocator_realloc(p->ctx->alloc, p->stack.is_block, new_cap * sizeof(bool));
		unsigned char *new_flow_flags = (unsigned char *)gtext_allocator_realloc(p->ctx->alloc, p->stack.flow_flags, new_cap);
		saved_temp *new_temps = (saved_temp *)gtext_allocator_realloc(p->ctx->alloc, p->stack.temps, new_cap * sizeof(saved_temp));
		
		if (!new_nodes || !new_states || !new_indents || !new_is_block ||
			!new_flow_flags || !new_temps) {
			gtext_allocator_free(p->ctx->alloc, new_nodes);
			gtext_allocator_free(p->ctx->alloc, new_states);
			gtext_allocator_free(p->ctx->alloc, new_indents);
			gtext_allocator_free(p->ctx->alloc, new_is_block);
			gtext_allocator_free(p->ctx->alloc, new_flow_flags);
			gtext_allocator_free(p->ctx->alloc, new_temps);
			return false;
		}
		
		/* Zero-initialize new slots */
		memset(new_temps + p->stack.capacity, 0, (new_cap - p->stack.capacity) * sizeof(saved_temp));
		memset(new_flow_flags + p->stack.capacity, 0, new_cap - p->stack.capacity);
		
		p->stack.nodes = new_nodes;
		p->stack.states = new_states;
		p->stack.indents = new_indents;
		p->stack.is_block = new_is_block;
		p->stack.flow_flags = new_flow_flags;
		p->stack.temps = new_temps;
		p->stack.capacity = new_cap;
	}
	
	/* Save current temp state to the stack */
	p->stack.temps[p->stack.depth].items = p->temp.items;
	p->stack.temps[p->stack.depth].count = p->temp.count;
	p->stack.temps[p->stack.depth].capacity = p->temp.capacity;
	p->stack.temps[p->stack.depth].anchor = gtext_yaml_strdup(anchor, p->ctx->alloc);
	p->stack.temps[p->stack.depth].tag = gtext_yaml_strdup(tag, p->ctx->alloc);
	p->stack.temps[p->stack.depth].source_offset = source_offset;
	p->stack.temps[p->stack.depth].source_line = source_line;
	p->stack.temps[p->stack.depth].source_col = source_col;
	
	/* Allocate new temp storage for this level */
	p->temp.capacity = 16;
	p->temp.items = (GTEXT_YAML_Node **)gtext_allocator_malloc(p->ctx->alloc, p->temp.capacity * sizeof(GTEXT_YAML_Node *));
	if (!p->temp.items) {
		/* Restore old temp on failure */
		p->temp.items = p->stack.temps[p->stack.depth].items;
		p->temp.count = p->stack.temps[p->stack.depth].count;
		p->temp.capacity = p->stack.temps[p->stack.depth].capacity;
		gtext_allocator_free(p->ctx->alloc, p->stack.temps[p->stack.depth].anchor);
		gtext_allocator_free(p->ctx->alloc, p->stack.temps[p->stack.depth].tag);
		p->stack.temps[p->stack.depth].anchor = NULL;
		p->stack.temps[p->stack.depth].tag = NULL;
		return false;
	}
	p->temp.count = 0;
	
	p->stack.nodes[p->stack.depth] = node;
	p->stack.states[p->stack.depth] = state;
	p->stack.indents[p->stack.depth] = indent;
	p->stack.is_block[p->stack.depth] = is_block;
	p->stack.flow_flags[p->stack.depth] = 0;
	p->stack.depth++;
	return true;
}

/**
 * @brief Pop a node from the stack.
 * Restores the saved temp state from the previous level.
 */
static void stack_pop(parser_state *p) {
	if (p->stack.depth > 0) {
		p->stack.depth--;
		
		/* Free current temp and restore saved temp state */
		gtext_allocator_free(p->ctx->alloc, p->temp.items);
		p->temp.items = p->stack.temps[p->stack.depth].items;
		p->temp.count = p->stack.temps[p->stack.depth].count;
		p->temp.capacity = p->stack.temps[p->stack.depth].capacity;
		
		/* Note: Don't free anchor/tag here - they're still needed for node creation */
		/* They'll be freed after creating the node in the event handler */
	}
}

/**
 * @brief Pop and get the saved anchor/tag, then clear them.
 */
static void stack_get_and_clear_metadata(
	parser_state *p,
	char **anchor,
	char **tag,
	size_t *source_offset,
	int *source_line,
	int *source_col
) {
	if (p->stack.depth > 0 && p->stack.depth <= p->stack.capacity) {
		size_t idx = p->stack.depth - 1;
		*anchor = p->stack.temps[idx].anchor;
		*tag = p->stack.temps[idx].tag;
		if (source_offset) *source_offset = p->stack.temps[idx].source_offset;
		if (source_line) *source_line = p->stack.temps[idx].source_line;
		if (source_col) *source_col = p->stack.temps[idx].source_col;
		p->stack.temps[idx].anchor = NULL;
		p->stack.temps[idx].tag = NULL;
		p->stack.temps[idx].source_offset = 0;
		p->stack.temps[idx].source_line = 0;
		p->stack.temps[idx].source_col = 0;
	} else {
		*anchor = NULL;
		*tag = NULL;
		if (source_offset) *source_offset = 0;
		if (source_line) *source_line = 0;
		if (source_col) *source_col = 0;
	}
}

static void node_set_source_location(
	GTEXT_YAML_Node *node,
	size_t offset,
	int line,
	int col
) {
	if (!node) return;
	switch (node->type) {
		case GTEXT_YAML_STRING:
		case GTEXT_YAML_BOOL:
		case GTEXT_YAML_INT:
		case GTEXT_YAML_FLOAT:
		case GTEXT_YAML_NULL:
			node->as.scalar.source_offset = offset;
			node->as.scalar.source_line = line;
			node->as.scalar.source_col = col;
			return;
		case GTEXT_YAML_SEQUENCE:
		case GTEXT_YAML_OMAP:
		case GTEXT_YAML_PAIRS:
			node->as.sequence.source_offset = offset;
			node->as.sequence.source_line = line;
			node->as.sequence.source_col = col;
			return;
		case GTEXT_YAML_MAPPING:
		case GTEXT_YAML_SET:
			node->as.mapping.source_offset = offset;
			node->as.mapping.source_line = line;
			node->as.mapping.source_col = col;
			return;
		case GTEXT_YAML_ALIAS:
			node->as.alias.source_offset = offset;
			node->as.alias.source_line = line;
			node->as.alias.source_col = col;
			return;
		default:
			return;
	}
}

static void node_get_source_location(
	const GTEXT_YAML_Node *node,
	size_t *offset,
	int *line,
	int *col
) {
	if (offset) *offset = 0;
	if (line) *line = 0;
	if (col) *col = 0;
	if (!node) return;
	switch (node->type) {
		case GTEXT_YAML_STRING:
		case GTEXT_YAML_BOOL:
		case GTEXT_YAML_INT:
		case GTEXT_YAML_FLOAT:
		case GTEXT_YAML_NULL:
			if (offset) *offset = node->as.scalar.source_offset;
			if (line) *line = node->as.scalar.source_line;
			if (col) *col = node->as.scalar.source_col;
			return;
		case GTEXT_YAML_SEQUENCE:
		case GTEXT_YAML_OMAP:
		case GTEXT_YAML_PAIRS:
			if (offset) *offset = node->as.sequence.source_offset;
			if (line) *line = node->as.sequence.source_line;
			if (col) *col = node->as.sequence.source_col;
			return;
		case GTEXT_YAML_MAPPING:
		case GTEXT_YAML_SET:
			if (offset) *offset = node->as.mapping.source_offset;
			if (line) *line = node->as.mapping.source_line;
			if (col) *col = node->as.mapping.source_col;
			return;
		case GTEXT_YAML_ALIAS:
			if (offset) *offset = node->as.alias.source_offset;
			if (line) *line = node->as.alias.source_line;
			if (col) *col = node->as.alias.source_col;
			return;
		default:
			return;
	}
}

/**
 * @brief Add a node to the temporary collection being built.
 */
static bool temp_add(parser_state *p, GTEXT_YAML_Node *node) {
	if (p->temp.count >= p->temp.capacity) {
		/* Grow temp storage */
		size_t new_cap = p->temp.capacity * 2;
		GTEXT_YAML_Node **new_items = (GTEXT_YAML_Node **)gtext_allocator_realloc(p->ctx->alloc, 
			p->temp.items, new_cap * sizeof(GTEXT_YAML_Node *)
		);
		if (!new_items) return false;
		
		p->temp.items = new_items;
		p->temp.capacity = new_cap;
	}
	
	p->temp.items[p->temp.count++] = node;
	return true;
}

/**
 * @brief Give a mapping key that has no value an explicit null one.
 *
 * A mapping's children are collected as a flat alternating key, value, key,
 * value list, and the pairs are formed by halving that list.  A key whose
 * value is absent therefore did not produce an empty value - it left the list
 * one short, so the halving dropped the last key and, worse, paired every
 * later key with the wrong value:
 *
 *     a:          gave {}              rather than {a: null}
 *     a:          gave {a: b}, losing  rather than {a: null, b: 1}
 *     b: 1          the 1
 *     ? a         gave {}              rather than {a: null}
 *     ? a         was a parse error    rather than {a: null, b: null}
 *     ? b
 *
 * YAML says an absent value is null, so the fix is to supply one.
 *
 * The supplied node is empty, because that is what was written. It used to
 * be "~": the resolver read a scalar's text without looking at how it was
 * written, so an empty scalar resolved to the empty *string* and "a:" could
 * not be told apart from "a: ''". The resolver consults the scalar's style
 * now - only a plain scalar is resolved by its contents (10.3.2) - so a
 * plain empty scalar reaches GTEXT_YAML_NULL on its own and the substitute
 * spelling is no longer needed to get there.
 *
 * Which matters because the spelling was visible. gtext_yaml_node_as_string()
 * returned "~" for a node whose author wrote nothing, and two null keys
 * written the same way came out as different JSON keys once coerced.
 */
static bool mapping_supply_null_value(parser_state *p) {
	GTEXT_YAML_Node *empty = yaml_node_new_scalar(p->ctx, "", 0, NULL, NULL);
	if (!empty) {
		return false;
	}
	return temp_add(p, empty);
}

/**
 * @brief Give a block sequence entry that holds nothing the null it stands for.
 *
 * An entry with no node is the empty node (7.2, e-node), which resolves to
 * null: "-" on its own is [null] and "- # comment" is an entry too. Both were
 * being dropped, so the sequence came back one entry short.
 */
static bool sequence_supply_empty_entry(parser_state *p) {
	GTEXT_YAML_Node *empty = yaml_node_new_scalar(p->ctx, "", 0, NULL, NULL);
	if (!empty) {
		return false;
	}
	return temp_add(p, empty);
}

/**
 * @brief Is the node at @p offset written on a document-start line?
 *
 * A block collection may not begin on the "---" line.  s-l+block-collection
 * reaches l+block-mapping(n) only through s-l-comments (8.2), and once the
 * line has content on it that needs a line break, so "--- a: b" has no
 * production.  A scalar is another matter - "--- a" and "--- !!str a" are
 * ordinary bare documents - so this asks only about the line and the caller
 * applies it where a block collection is about to start.
 *
 * yaml-test-suite has both spellings as errors: CXX2 "--- &anchor a: b" and
 * 9KBC "--- key1: value1" with a second key below it.  PyYAML refuses them
 * too.  js-yaml accepts the plain "--- a: b" and refuses the rest, which is
 * the more forgiving reading; this follows the grammar.
 */
/**
 * @brief Is this ":" the first thing on its line?
 *
 * A block mapping entry begins at s-indent(n), so an entry whose key is the
 * empty node has nothing in front of its ":" but the indentation (8.2.2).
 *
 * Anything else in front of it belongs to whatever that is.  In
 *
 *     a:
 *     - 1
 *       b: 2
 *
 * "1 b" is one plain scalar across two lines and the ":" after it ends
 * nothing, which is an error - not a mapping entry with no key.
 */
static bool colon_begins_its_line(const parser_state *p, size_t offset) {
	const char *buffer = NULL;
	size_t i = 0;

	if (!p || !p->ctx || !p->ctx->decoded_input) return false;
	buffer = p->ctx->decoded_input;
	/* An offset past the end of the buffer means the end of it, and the
	   question - what stands between here and the start of the line - is the
	   same one.  This scans *backwards* from the offset, so without the clamp
	   the first read is off the end of the allocation: ASan called it a
	   heap-buffer-overflow, 22 bytes before whatever the allocator had put
	   next.  The five other helpers that index this buffer have all had this
	   line since they were written; this one never did. */
	i = offset > p->ctx->decoded_input_len ? p->ctx->decoded_input_len : offset;
	while (i > 0) {
		char c = buffer[i - 1];
		if (c == '\n' || c == '\r') return true;
		if (c == ' ' || c == '\t') { i--; continue; }
		/* "- " opens a sequence entry and "? " an explicit key, and a compact
		   collection may follow either on the same line (8.2.1, 8.2.2).  So
		   may an entry with no key: "- :" is [{null: null}].  The space after
		   the indicator is what makes it one - "-:" is a plain scalar. */
		if ((c == '-' || c == '?')
				&& (i == 1 || buffer[i - 2] == '\n' || buffer[i - 2] == '\r'
					|| buffer[i - 2] == ' ' || buffer[i - 2] == '\t')) {
			i--;
			continue;
		}
		return false;
	}
	return true;
}

static bool node_is_on_document_start_line(const parser_state *p, size_t offset) {
	const char *buffer = NULL;
	size_t length = 0;
	size_t line_start = 0;

	if (!p || !p->ctx || !p->ctx->decoded_input) return false;
	buffer = p->ctx->decoded_input;
	length = p->ctx->decoded_input_len;
	if (offset > length) offset = length;

	line_start = yaml_context_line_start(p->ctx, offset);

	if (length - line_start < 4) return false;
	if (buffer[line_start] != '-' || buffer[line_start + 1] != '-'
		|| buffer[line_start + 2] != '-') {
		return false;
	}
	return buffer[line_start + 3] == ' ' || buffer[line_start + 3] == '\t';
}

/**
 * @brief True when a node starting at @p col is a sibling key rather than the
 *        value of the key already waiting for one.
 *
 * Indentation decides it.  Content indented past the key belongs to the key:
 *
 *     x:            {x: {y: 1}}
 *       y: 1
 *
 * Content at the key's own column is the next key, so the previous one had no
 * value:
 *
 *     a:            {a: null, b: 1}
 *     b: 1
 *
 * Scalars, aliases and flow collections are asked.  A block sequence may sit
 * at its key's own column and still be that key's value - "a:" then "- 1" is
 * {a: [1]} - so the '-' indicator must not be treated this way.
 */
/**
 * @brief The column a node begins in, which is not always the column of its
 *        content.
 *
 * Properties are held by the stream and reported with the event they end up
 * on, so "&anchor c" arrives at the column of the "c".  Under an empty "b:"
 * that put the node eight columns past the mapping and it was taken for b's
 * value rather than the next key.  A property left on a line of its own
 * introduces whatever follows it and is a different question, judged by
 * property_left_of_open_collection().
 */
static int event_node_col(const GTEXT_YAML_Event *event) {
	if (!event) return 0;
	if (event->prop_col >= 0 && event->prop_line == event->line
			&& event->prop_col < event->col) {
		return event->prop_col;
	}
	return event->col;
}

/**
 * @brief The column an alias node begins in.
 *
 * event_node_col() answers for a node whose content stands where the event
 * points, and an ALIAS event points at the first character of the anchor
 * name: the "*" that introduces it is one column to its left, and
 * c-ns-alias-node (7.1) admits nothing between the two.  One column is the
 * whole difference between "at the mapping's key column" and "one past it",
 * which is the question block_value_is_missing() asks.
 */
static int alias_node_col(const GTEXT_YAML_Event *event) {
	int col;
	if (!event) return 0;
	col = event_node_col(event);
	/* Unchanged where a property already moved it left: the "&" or "!" is
	   the node's first character and nothing precedes it. */
	if (col == event->col && col > 1) col--;
	return col;
}

static bool block_value_is_missing(parser_state *p, int col) {
	if (p->stack.depth == 0) return false;
	size_t top = p->stack.depth - 1;
	if (p->stack.states[top] != STATE_MAPPING_VALUE) return false;
	if (!p->stack.is_block[top]) return false;
	if (p->stack.indents[top] < 0) return false;
	return col <= p->stack.indents[top];
}

/**
 * @brief Give the key above its null value when this collection is the next key.
 *
 * A flow collection standing at the key's own column while a value is still
 * expected is the next entry's key rather than that value:
 *
 *     a:
 *     {}: 1
 *
 * is two entries, not one whose value is "{}".  The scalar case has had this
 * rule for as long as the block mapping has; the four places a completed
 * collection is added to its parent did not, so the flow mapping went where
 * a's value goes, and the ":" after it then found an even list in front of
 * it with no key of its own left to claim.
 *
 * A *block* sequence is the exception, and the reason this asks which style
 * the collection is: it may stand at its own key's column and still be that
 * key's value, which is what "key:" over "- a" means (8.2.1).  A flow
 * collection has no such spelling - indented it is the value, at the key's
 * column it is the next key - and both references agree.
 */
static int line_key_col_from_offset(const parser_state *p, size_t offset);

/* Whether the node at @p offset is the first thing on its line.
 *
 * "First" allows indentation, the "-", "?" and ":" of the entries that
 * contain it compactly, and the node's own properties - nothing else.  An
 * indicator counts as one only where white space follows it: "- x" is an
 * entry holding x, while "-: 1" is a mapping whose key is the plain scalar
 * "-".  block_key_may_start_at() skips those characters without that test,
 * which is deliberate where it is used - an alias event's offset points past
 * its "*" - but wrong here: it made "-: {a: 1}" look like a flow mapping
 * standing at the start of its line, so the collection was taken for the next
 * entry's key and the value it really was went missing.
 */
static bool block_node_begins_line(const parser_state *p, size_t offset) {
	const char *buffer = NULL;
	size_t line_start = 0;
	size_t i = 0;

	if (!p || !p->ctx || !p->ctx->decoded_input) return false;
	if (offset > p->ctx->decoded_input_len) return false;
	buffer = p->ctx->decoded_input;

	line_start = yaml_context_line_start(p->ctx, offset);

	i = line_start;
	while (i < offset && (buffer[i] == ' ' || buffer[i] == '\t')) i++;
	while (i + 1 < offset
			&& (buffer[i] == '-' || buffer[i] == '?' || buffer[i] == ':')
			&& (buffer[i + 1] == ' ' || buffer[i + 1] == '\t')) {
		i++;
		while (i < offset && (buffer[i] == ' ' || buffer[i] == '\t')) i++;
	}
	/* An anchor and a tag, in either order, each running to white space. */
	for (int prop = 0; prop < 2 && i < offset; ++prop) {
		if (buffer[i] != '&' && buffer[i] != '!') break;
		while (i < offset && buffer[i] != ' ' && buffer[i] != '\t') i++;
		while (i < offset && (buffer[i] == ' ' || buffer[i] == '\t')) i++;
	}
	return i >= offset;
}

/* Where the entry a node at @p offset begins, for comparing against a block
   mapping's indentation.

   Usually that is the node's own column.  It is further left exactly when the
   node begins its own line and carries properties, because a property is part
   of the key it precedes: "&a [x]" is a key starting at the "&".  Asking
   line_key_col_from_offset() unconditionally instead answers about the line's
   *first* node, which for "a: [b, c]" is the "a" - and that made the flow
   sequence look like the next key rather than a's value. */
static int block_entry_col(const parser_state *p, size_t offset, int node_col) {
	int col;
	if (!block_node_begins_line(p, offset)) return node_col;
	col = line_key_col_from_offset(p, offset);
	return col >= 0 ? col : node_col;
}

static GTEXT_YAML_Status block_value_supply_if_missing(
	parser_state *p,
	const GTEXT_YAML_Node *node,
	int col
) {
	GTEXT_YAML_Flow_Style style;

	if (!node) return GTEXT_YAML_OK;
	switch (node->type) {
		case GTEXT_YAML_SEQUENCE:
		case GTEXT_YAML_OMAP:
		case GTEXT_YAML_PAIRS:
			style = node->as.sequence.flow_style;
			break;
		case GTEXT_YAML_MAPPING:
		case GTEXT_YAML_SET:
			style = node->as.mapping.flow_style;
			break;
		default:
			return GTEXT_YAML_OK;
	}
	if (style != GTEXT_YAML_FLOW_STYLE_FLOW) return GTEXT_YAML_OK;
	if (!block_value_is_missing(p, col)) return GTEXT_YAML_OK;
	if (!mapping_supply_null_value(p)) {
		p->failed = true;
		if (p->error) {
			p->error->code = GTEXT_YAML_E_OOM;
			p->error->message = "Out of memory completing mapping value";
		}
		return GTEXT_YAML_E_OOM;
	}
	p->stack.states[p->stack.depth - 1] = STATE_MAPPING_KEY;
	return GTEXT_YAML_OK;
}


static int line_key_col_from_offset(const parser_state *p, size_t offset) {
	const char *buffer = NULL;
	size_t length = 0;
	size_t line_start = 0;
	size_t end = 0;
	size_t i = 0;
	int col = 1;

	if (!p || !p->ctx || !p->ctx->decoded_input) return -1;

	buffer = p->ctx->decoded_input;
	length = p->ctx->decoded_input_len;
	if (offset > length) offset = length;

	line_start = yaml_context_line_start(p->ctx, offset);

	end = offset;
	for (i = line_start; i < end; i++) {
		char ch = buffer[i];
		if (ch == ' ' || ch == '\t') {
			col++;
			continue;
		}
		break;
	}

	/* A compact entry puts its node after one or more "-", "?" or ":"
	 * indicators on the same line, and the node begins where they end:
	 * "- b: c" has b at column 2, and "? a" over ": b: c" has b at column 3.
	 * Only the "-" was being stepped over, so an explicit key's value colon
	 * left the following key measured at the colon's own column - it then
	 * matched the mapping's indentation and "? a" over ": b: c" was refused
	 * for having no key before the second colon. */
	while (i < end && (buffer[i] == '-' || buffer[i] == '?' || buffer[i] == ':')) {
		size_t j = i + 1;
		int col_after = col + 1;
		if (j >= end || (buffer[j] != ' ' && buffer[j] != '\t')) break;
		while (j < end && (buffer[j] == ' ' || buffer[j] == '\t')) {
			j++;
			col_after++;
		}
		i = j;
		col = col_after;
	}

	return col;
}

/**
 * @brief Whether a block collection entry may begin at @p offset.
 *
 * A block entry is preceded on its line by indentation and, where entries
 * nest compactly, by the "-" or "?" of the entries that contain it:
 * l+block-sequence is ( s-indent(n+m) c-l-block-seq-entry(n+m) )+, and a
 * compact sequence or mapping may follow a "-" on the same line. Anything
 * else on the line means a node has already been written there, and a second
 * one cannot start beside it.
 *
 * That was not being checked, so four shapes went through: "key: - a" put a
 * block sequence on the same line as the key that owns it (a block
 * collection as a mapping value has to start on the next line -
 * s-l+block-collection has s-l-comments in front of it), "&anchor - x"
 * attached an anchor to a sequence indicator, and "- { y: z }- invalid" and
 * "x: { y: z }in: valid" started a second node beside a finished flow
 * collection.
 */
static bool block_entry_may_start_at(const parser_state *p, size_t offset) {
	const char *buffer = NULL;
	size_t length = 0;
	size_t line_start = 0;

	if (!p || !p->ctx || !p->ctx->decoded_input) return true;
	buffer = p->ctx->decoded_input;
	length = p->ctx->decoded_input_len;
	if (offset > length) return true;

	line_start = yaml_context_line_start(p->ctx, offset);

	for (size_t i = line_start; i < offset; i++) {
		const char ch = buffer[i];
		/* A ":" is allowed only where everything before it was itself
		   indentation or an entry indicator, which is exactly an explicit
		   key's value colon - c-l-block-map-explicit-value(n) is
		   s-indent(n) ":" s-l+block-indented(n), so "? a" over ": - b" opens
		   a compact sequence. In "key: - a" the "k" is reached first and the
		   scan has already returned. */
		if (ch == ' ' || ch == '\t' || ch == '-' || ch == '?' || ch == ':') continue;
		return false;
	}
	return true;
}

/**
 * @brief The same question for a mapping key, which may carry properties.
 *
 * An anchor or a tag belongs to the node it precedes and sits in front of it
 * on the line - "!!str true: 2" is a tagged key - so those are allowed here
 * where they are not in front of a "-". A "&anchor" before a sequence
 * indicator has no node to attach to, which is what makes "&anchor - x"
 * malformed while "&anchor x: 1" is fine.
 */
static bool block_key_may_start_at(const parser_state *p, size_t offset) {
	const char *buffer = NULL;
	size_t i = 0;
	size_t line_start = 0;

	if (!p || !p->ctx || !p->ctx->decoded_input) return true;
	buffer = p->ctx->decoded_input;
	if (offset > p->ctx->decoded_input_len) return true;

	line_start = yaml_context_line_start(p->ctx, offset);

	i = line_start;
	/* "*" is in the run because an alias event's offset points at the name
	   rather than at the indicator, and an alias may stand as a key:
	   "*b : *a" is a mapping whose key is whatever &b named (7.1). */
	while (i < offset && (buffer[i] == ' ' || buffer[i] == '\t'
			|| buffer[i] == '-' || buffer[i] == '?' || buffer[i] == ':'
			|| buffer[i] == '*')) {
		i++;
	}
	/* An anchor and a tag, in either order, each running to white space. */
	for (int prop = 0; prop < 2 && i < offset; ++prop) {
		if (buffer[i] != '&' && buffer[i] != '!') break;
		while (i < offset && buffer[i] != ' ' && buffer[i] != '\t') i++;
		while (i < offset && (buffer[i] == ' ' || buffer[i] == '\t')) i++;
	}
	return i >= offset;
}

static bool stack_top_is_block(const parser_state *p) {
	if (!p || p->stack.depth == 0) return false;
	return p->stack.is_block[p->stack.depth - 1];
}

static int stack_top_indent(const parser_state *p) {
	if (!p || p->stack.depth == 0) return -1;
	return p->stack.indents[p->stack.depth - 1];
}


/**
 * @brief Move an own-line tag from a scalar onto the collection it begins.
 *
 * A tag applies to the node that follows it.  The scanner can only attach a
 * pending tag to the next scalar, because in block context nothing yet says
 * whether the node starting there is that scalar or a collection whose first
 * key or item it is.  Once the parser has decided, this puts the tag where it
 * belongs.
 *
 * Only own-line tags move.  "!custom a: 1" tags the key and "!custom" on its
 * own line then "a: 1" tags the mapping; the two differ in nothing but where
 * the tag was written, which is why GTEXT_YAML_Event carries tag_line.
 *
 * The collection's tag lives in the stack entry until the node is created at
 * its end, which is the same slot a flow collection's tag arrives in.
 */
static GTEXT_YAML_Status adopt_own_line_tag(
		parser_state *p, GTEXT_YAML_Node *node) {
	if (!p || !node) return GTEXT_YAML_OK;
	if (p->stack.depth == 0) return GTEXT_YAML_OK;

	/* A tag the stream could not place is this collection's: it was written
	 * before the one the node carries, so the node is inside it.
	 *
	 * The node's own tag may then belong to a collection too - a deeper one,
	 * not yet pushed - because more than one can open in a row.  Spec
	 * example 2.24 does exactly that:
	 *
	 *     --- !shape        !shape is the sequence's, !circle the first
	 *     - !circle         entry's mapping's, and that mapping does not
	 *       center: 1       open until the ":" two tokens later
	 *
	 * So the node's tag goes into the slot the outer one just left, and
	 * waits for that push.  If none comes, it named the collection filled
	 * here after all - which would then have two - and the check between
	 * events refuses it:
	 *
	 *     top: !!seq        !!seq and !!map both name the sequence
	 *       !!map
	 *       - 1
	 */
	if (p->outer_tag) {
		p->outer_from_alias = false;
		size_t held = p->stack.depth - 1;
		if (!p->stack.temps[held].tag) {
			p->stack.temps[held].tag = p->outer_tag;
		} else {
			gtext_allocator_free(p->ctx->alloc, p->outer_tag);
		}
		p->outer_tag = NULL;

		if (p->last_scalar_tag_own_line && node->type < GTEXT_YAML_SEQUENCE
				&& node->as.scalar.tag) {
			p->outer_tag = gtext_yaml_strdup(node->as.scalar.tag, p->ctx->alloc);
			if (!p->outer_tag) return GTEXT_YAML_OK; /* keep it on the node */
			node->as.scalar.tag = NULL;
			p->last_scalar_tag_own_line = false;
		}
		return GTEXT_YAML_OK;
	}

	if (!p->last_scalar_tag_own_line) return GTEXT_YAML_OK;
	/* The DOM has no single scalar type: NULL through STRING are the scalar
	 * kinds, and SEQUENCE onwards are collections and aliases. */
	if (node->type >= GTEXT_YAML_SEQUENCE) return GTEXT_YAML_OK;

	const char *tag = node->as.scalar.tag;
	if (!tag) return GTEXT_YAML_OK;

	size_t top = p->stack.depth - 1;
	/* the collection already has one */
	if (p->stack.temps[top].tag) return GTEXT_YAML_OK;

	char *moved = gtext_yaml_strdup(tag, p->ctx->alloc);
	/* leaving the tag on the scalar beats losing it */
	if (!moved) return GTEXT_YAML_OK;

	p->stack.temps[top].tag = moved;
	node->as.scalar.tag = NULL;
	p->last_scalar_tag_own_line = false;
	return GTEXT_YAML_OK;
}

/**
 * @brief Move an own-line anchor from the first scalar to its collection.
 *
 * The same rule as adopt_own_line_tag(), for the other half of a node's
 * properties, and it was never applied. An anchor on a *flow* collection
 * arrives with the "[" or "{" and reaches the collection; a block one has no
 * such token, so the anchor stayed on whichever scalar the stream could
 * attach it to - the first entry of a sequence, or the first key of a
 * mapping. Spec example 2.27 is the plain case:
 *
 *     bill-to: &id001
 *         given  : Chris
 *         ...
 *     ship-to: *id001
 *
 * where *id001 came back as the string "given" rather than the mapping
 * (suite case UGM3). "x: &anc" over "  - 1" is the sequence version, whose
 * alias gave 1 instead of [1].
 *
 * The scalar has already registered the anchor by the time this runs, and
 * that registration has to be taken back as well as the anchor string -
 * unregister_anchor() says why. Moving the string alone left an alias
 * *inside* the collection bound to the scalar, because the entry the alias
 * looked the name up in still named it.
 *
 * The three tests before the work are preconditions rather than behaviour,
 * and mirror adopt_own_line_tag() line for line. Instrumenting them shows no
 * input reaching any of the three: today's call sites always have a level on
 * the stack and always pass a scalar, and the levels they pass have no anchor
 * of their own. Two of them are what makes the function safe to call at all -
 * temps[depth - 1] at depth zero is out of bounds, and as.scalar.anchor on a
 * collection reads the wrong member of the union - so they stay.
 */
static GTEXT_YAML_Status adopt_own_line_anchor(
		parser_state *p, GTEXT_YAML_Node *node) {
	if (!p || !node) return GTEXT_YAML_OK;
	if (p->stack.depth == 0) return GTEXT_YAML_OK;

	/* The same handover adopt_own_line_tag() describes, for anchors:
	 *
	 *     top1: &node1        &node1 is the mapping's, &k1 the key's, and
	 *       &k1 key1: val1    both arrive on the key
	 */
	if (p->outer_anchor) {
		p->outer_from_alias = false;
		size_t held = p->stack.depth - 1;
		if (!p->stack.temps[held].anchor) {
			p->stack.temps[held].anchor = p->outer_anchor;
		} else {
			gtext_allocator_free(p->ctx->alloc, p->outer_anchor);
		}
		p->outer_anchor = NULL;

		if (p->last_scalar_anchor_own_line && node->type < GTEXT_YAML_SEQUENCE
				&& node->as.scalar.anchor) {
			p->outer_anchor = gtext_yaml_strdup(node->as.scalar.anchor, p->ctx->alloc);
			if (!p->outer_anchor) return GTEXT_YAML_OK; /* keep it on the node */
			unregister_anchor(p, node->as.scalar.anchor, node);
			node->as.scalar.anchor = NULL;
			p->last_scalar_anchor_own_line = false;
		}
		return GTEXT_YAML_OK;
	}

	if (!p->last_scalar_anchor_own_line) return GTEXT_YAML_OK;
	if (node->type >= GTEXT_YAML_SEQUENCE) return GTEXT_YAML_OK;

	const char *anchor = node->as.scalar.anchor;
	if (!anchor) return GTEXT_YAML_OK;

	size_t top = p->stack.depth - 1;
	/* the collection already has one */
	if (p->stack.temps[top].anchor) return GTEXT_YAML_OK;

	char *moved = gtext_yaml_strdup(anchor, p->ctx->alloc);
	/* leaving it on the scalar beats losing it */
	if (!moved) return GTEXT_YAML_OK;

	p->stack.temps[top].anchor = moved;
	unregister_anchor(p, anchor, node);
	node->as.scalar.anchor = NULL;
	p->last_scalar_anchor_own_line = false;
	return GTEXT_YAML_OK;
}

/**
 * @brief Take custody of a property the stream could not place.
 *
 * The stream sets a property aside in its outer_* slots when a second one of
 * its kind arrives on a later line, because a block collection may open
 * between the two and then they name two nodes rather than one
 * (stream_defer_property()). It travels on every node event from there, and
 * the node that finally settles it is the one adopt_own_line_anchor() hands
 * it to.
 *
 * Every event that can carry a node has to pick it up, not just SCALAR. A
 * mapping whose first key is a flow collection reaches the parser as
 * SEQUENCE_START, and one whose first key is an alias as ALIAS; both used to
 * let the property fall on the floor:
 *
 *     &mapping                 &mapping is the block mapping's and &key the
 *     &key [ &item a, b, c ]: value      sequence's (suite case 6BFJ)
 *
 * The slots are empty on arrival: parse_callback refuses anything the node
 * before left behind.
 */
static GTEXT_YAML_Status parser_hold_outer_props(
		parser_state *p, const GTEXT_YAML_Event *event) {
	if (!p || !event) return GTEXT_YAML_OK;
	if (!event->outer_anchor && !event->outer_tag) return GTEXT_YAML_OK;

	p->outer_from_alias = event->type == GTEXT_YAML_EVENT_ALIAS;

	if (event->outer_anchor && !p->outer_anchor) {
		p->outer_anchor = gtext_yaml_strdup(event->outer_anchor, p->ctx->alloc);
		if (!p->outer_anchor) {
			p->failed = true;
			if (p->error) {
				p->error->code = GTEXT_YAML_E_OOM;
				p->error->message = "Out of memory holding an anchor";
			}
			return GTEXT_YAML_E_OOM;
		}
	}
	if (event->outer_tag && !p->outer_tag) {
		p->outer_tag = gtext_yaml_strdup(event->outer_tag, p->ctx->alloc);
		if (!p->outer_tag) {
			p->failed = true;
			if (p->error) {
				p->error->code = GTEXT_YAML_E_OOM;
				p->error->message = "Out of memory holding a tag";
			}
			return GTEXT_YAML_E_OOM;
		}
	}
	return GTEXT_YAML_OK;
}

static void maybe_finish_block_mapping_value(parser_state *p) {
	if (!p || p->stack.depth == 0) return;
	if (!p->stack.is_block[p->stack.depth - 1]) return;
	if (p->stack.states[p->stack.depth - 1] != STATE_MAPPING_VALUE) return;

	p->stack.states[p->stack.depth - 1] = STATE_MAPPING_KEY;
}

/**
 * @brief Make @p node the document root, or refuse a second one.
 *
 * A document has exactly one root node (3.2.1). Every site that finished a
 * node at stack depth zero simply assigned it, so a second top-level node
 * overwrote the first and the first was gone: "- a\n- b\ninvalid: x"
 * returned {"invalid": "x"} with the sequence dropped, and "word1 # comment"
 * over "word2" returned just "word2". Silent data loss rather than a
 * refusal, which is the shape most of the defects in this parser have had.
 *
 * detach_last_scalar() clears the root when a scalar taken provisionally as
 * the root turns out to be a mapping key, so the ordinary "a: 1" path still
 * reaches here with no root set.
 */
static GTEXT_YAML_Status set_document_root(parser_state *p, GTEXT_YAML_Node *node) {
	if (p->root) {
		if (p->error) {
			p->error->code = GTEXT_YAML_E_INVALID;
			p->error->message = "Second top-level node in one document";
		}
		p->failed = true;
		return GTEXT_YAML_E_INVALID;
	}
	p->root = node;
	return GTEXT_YAML_OK;
}

/**
 * @brief Take back a flow collection that turns out to be a mapping key.
 *
 * ns-s-block-map-implicit-key is c-s-implicit-json-key or
 * ns-s-implicit-yaml-key (8.2.2), and the first of those is a
 * c-flow-json-node: "[a]: b" and "{a: 1}: b" are block mappings whose key is
 * the flow collection.  detach_last_scalar() cannot answer for them because
 * the node is not a scalar, and every such document was refused.
 *
 * Only a *flow* collection qualifies, and only one that finished on this
 * line: a block collection has no c-flow-json-node to be, and an implicit key
 * is on one line by construction.  The style is the one the parser recorded
 * when it built the node.
 */
static GTEXT_YAML_Node *flow_key_candidate(const parser_state *p, int line) {
	GTEXT_YAML_Node *node = p->last_emitted_node;
	GTEXT_YAML_Flow_Style style;

	if (!node || p->last_emitted_line != line) return NULL;
	switch (node->type) {
		case GTEXT_YAML_SEQUENCE:
		case GTEXT_YAML_OMAP:
		case GTEXT_YAML_PAIRS:
			style = node->as.sequence.flow_style;
			break;
		case GTEXT_YAML_MAPPING:
		case GTEXT_YAML_SET:
			style = node->as.mapping.flow_style;
			break;
		default:
			return NULL;
	}
	if (style != GTEXT_YAML_FLOW_STYLE_FLOW) return NULL;

	/* It has to still be where it was put, so that taking it back cannot
	   disturb anything built since. */
	if (p->root == node) return node;
	if (p->temp.count > 0 && p->temp.items[p->temp.count - 1] == node) return node;
	return NULL;
}

static GTEXT_YAML_Node *detach_last_flow_node(parser_state *p, int line) {
	GTEXT_YAML_Node *node = flow_key_candidate(p, line);

	if (!node) return NULL;
	if (p->root == node) {
		p->root = NULL;
	}
	else {
		p->temp.count--;
	}
	p->last_emitted_node = NULL;
	return node;
}

/**
 * @brief Take back the last scalar, because a ":" makes it a mapping key.
 *
 * Where the scalar was put decides how to take it back.  The temp case checks
 * the thing it describes - right depth, non-empty, and that node on top - and
 * declines otherwise; the root case checked only a flag.
 *
 * **The flag outlives the scalar.**  It is set when a scalar becomes the root
 * and is not cleared when a ":" claims that scalar, and the paths that hold a
 * scalar as an *explicit* key ("? x") assign last_scalar_node without
 * touching either placement flag.  So a later scalar could arrive still
 * carrying an earlier one's "I am the root" and clear a p->root that had since
 * been filled by a finished collection:
 *
 *     &O
 *      1:
 *     ? *O
 *
 * The dedent to column 1 closes the indent-2 mapping onto the root; the ":"
 * after the explicit key then discarded it.  " 1:" over "? 2:" is the same
 * thing without the anchor, and parsed as {1: {2: null}} - two top-level
 * collections silently folded into one, where PyYAML refuses a second
 * top-level node outright.  With "&O" on the node that is discarded it is
 * worse than a wrong tree: the anchor leaves the DOM and the alias stays in
 * it, so the writer emits "*O" against no "&O" anywhere and produces a
 * document its own parser refuses.  A twelve-hour fuzz run over the writers
 * reached that in 3.4 million executions.
 *
 * Testing identity rather than the flag is the whole fix, and it is the test
 * detach_last_flow_node() has always made one function above.  Trying instead
 * to keep the flags truthful on the explicit-key paths looked more thorough
 * and was wrong twice over: a captured explicit key is in temp but already
 * claimed, so calling it "in temp" let a same-line ":" take back a key that
 * was spoken for ("? !!str : 1" gained an empty key), and calling it "in
 * neither" broke the compact mapping an explicit key is allowed to be
 * ("? earth: blue").  The placement flags are left exactly as they were.
 */
static GTEXT_YAML_Node *detach_last_scalar(parser_state *p) {
	GTEXT_YAML_Node *node = p->last_scalar_node;

	if (!node) return NULL;

	if (p->last_scalar_in_root && p->root == node) {
		p->root = NULL;
		p->last_scalar_node = NULL;
		p->last_scalar_in_root = false;
		return node;
	}

	if (!p->last_scalar_in_temp) return NULL;
	if (p->last_scalar_temp_depth != p->stack.depth) return NULL;
	if (p->temp.count == 0) return NULL;
	if (p->temp.items[p->temp.count - 1] != node) return NULL;

	p->temp.count--;
	p->last_scalar_node = NULL;
	return node;
}

static GTEXT_YAML_Status capture_explicit_key(
	parser_state *p,
	GTEXT_YAML_Node *node,
	bool *handled
) {
	if (!handled) return GTEXT_YAML_E_INVALID;
	*handled = false;

	if (!p || !node) return GTEXT_YAML_E_INVALID;
	if (!p->explicit_key_pending) return GTEXT_YAML_OK;
	if (p->stack.depth != p->explicit_key_depth) return GTEXT_YAML_OK;
	if (p->stack.depth == 0) {
		if (p->error) {
			p->error->code = GTEXT_YAML_E_INVALID;
			p->error->message = "Explicit key missing mapping context";
		}
		return GTEXT_YAML_E_INVALID;
	}

	size_t top = p->stack.depth - 1;
	if (p->stack.states[top] != STATE_MAPPING_KEY &&
		p->stack.states[top] != STATE_MAPPING_VALUE) {
		if (p->error) {
			p->error->code = GTEXT_YAML_E_INVALID;
			p->error->message = "Explicit key used outside mapping";
		}
		return GTEXT_YAML_E_INVALID;
	}

	/* An explicit key creates its mapping in the '?' handler rather than at a
	 * ':', and arrives here rather than through the ordinary scalar path, so
	 * an own-line tag before it needs adopting here too.  Only the first key:
	 * a later one cannot retag a mapping that already exists. */
	const bool first_key = (p->temp.count == 0);

	if (!temp_add(p, node)) {
		if (p->error) {
			p->error->code = GTEXT_YAML_E_OOM;
			p->error->message = "Out of memory adding explicit key";
		}
		return GTEXT_YAML_E_OOM;
	}
	/* A "?" claims its key as surely as a ":" does: an explicit key with no
	 * ":" after it is the block spelling of a set. */
	p->claimed_key = node;

	if (first_key) {
		GTEXT_YAML_Status adopted = adopt_own_line_tag(p, node);
		if (adopted == GTEXT_YAML_OK) adopted = adopt_own_line_anchor(p, node);
		if (adopted != GTEXT_YAML_OK) return adopted;
	}

	p->explicit_key_pending = false;
	p->explicit_key_active = true;
	p->stack.states[top] = STATE_MAPPING_KEY;
	*handled = true;
	return GTEXT_YAML_OK;
}

static void flow_entry_completed(parser_state *p);
static GTEXT_YAML_Status flow_entry_needs_separator(parser_state *p);

/**
 * @brief Close a mapping whose last key has no value.
 *
 * "a:" at the end of a document is {"a": null}, and so is "? a" with no ":"
 * after it, so an odd number of children is ordinarily just a trailing key.
 * What it must not be is a scalar that nothing ever claimed as a key: "top1:"
 * over "  key1: val1" over "top2" was giving {"top1": {...}, "top2": null},
 * inventing a pair out of a line that is simply malformed. The claimed_key
 * pointer says which of the two this is.
 *
 * Only in block context: "{a}" really is {"a": null}.
 *
 * On OOM the key is dropped as it was before, which is the old behavior
 * rather than a new one.
 */
/**
 * @brief Give a key that holds nothing the empty node it stands for.
 *
 * The same node a missing value gets - e-node either way (7.2) - under the
 * name that says which half of the pair is missing here.
 */
static bool mapping_supply_empty_key(parser_state *p) {
	return mapping_supply_null_value(p);
}

static GTEXT_YAML_Status mapping_close_trailing_key(parser_state *p) {
	/* A "?" whose key never arrived still made an entry, and both halves of
	 * it are the empty node: ns-flow-map-explicit-entry is
	 * "ns-flow-map-implicit-entry | ( e-node e-node )" (7.4), so "{ ? }" is
	 * {null: null}.  Dropping the key left the mapping empty instead. */
	if (p->explicit_key_pending
			&& p->stack.depth > 0
			&& p->explicit_key_depth == p->stack.depth
			&& (p->temp.count % 2) == 0) {
		if (!mapping_supply_empty_key(p)) {
			p->failed = true;
			if (p->error) {
				p->error->code = GTEXT_YAML_E_OOM;
				p->error->message = "Out of memory completing explicit key";
			}
			return GTEXT_YAML_E_OOM;
		}
		p->explicit_key_pending = false;
	}
	/* A "?" that got its key but never a ":" has a value all the same:
	 * c-l-block-map-explicit-entry's second arm is e-node (8.2.2), so
	 * "? - a" with nothing under it is {[a]: null}.  That key was claimed by
	 * the "?" rather than by a ":", so the check below - which asks whether a
	 * ":" claimed it - would call it a scalar nobody wanted. */
	if (p->explicit_key_active
			&& p->stack.depth > 0
			&& p->explicit_key_depth == p->stack.depth
			&& (p->temp.count % 2) == 1) {
		(void)mapping_supply_null_value(p);
		p->explicit_key_active = false;
		return GTEXT_YAML_OK;
	}
	if ((p->temp.count % 2) == 0) return GTEXT_YAML_OK;
	if (p->stack.depth > 0 && p->stack.is_block[p->stack.depth - 1]
			&& p->temp.items[p->temp.count - 1] != p->claimed_key) {
		p->failed = true;
		if (p->error) {
			p->error->code = GTEXT_YAML_E_INVALID;
			p->error->message = "Scalar with no ':' in a block mapping";
		}
		return GTEXT_YAML_E_INVALID;
	}
	(void)mapping_supply_null_value(p);
	return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status finalize_top_collection(parser_state *p) {
	GTEXT_YAML_Node *node = NULL;
	char *anchor = NULL;
	char *tag = NULL;
	size_t source_offset = 0;
	int source_line = 0;
	int source_col = 0;
	int state = 0;

	if (!p || p->stack.depth == 0) return GTEXT_YAML_OK;

	state = p->stack.states[p->stack.depth - 1];
	/* Read before stack_pop() below, which is the only other place this
	   level's style is still known. */
	const GTEXT_YAML_Flow_Style flow_style = p->stack.is_block[p->stack.depth - 1]
		? GTEXT_YAML_FLOW_STYLE_BLOCK
		: GTEXT_YAML_FLOW_STYLE_FLOW;

	stack_get_and_clear_metadata(
		p,
		&anchor,
		&tag,
		&source_offset,
		&source_line,
		&source_col
	);

	if (state == STATE_SEQUENCE) {
		/* A trailing "-" with nothing under it is an entry all the same. */
		if (p->stack.is_block[p->stack.depth - 1]
				&& (p->stack.flow_flags[p->stack.depth - 1]
					& GTEXT_YAML_BLOCK_ENTRY_OPEN)) {
			(void)sequence_supply_empty_entry(p);
		}
		node = yaml_node_new_sequence(p->ctx, p->temp.count, tag, anchor);
		if (!node) {
			gtext_allocator_free(p->ctx->alloc, anchor);
			gtext_allocator_free(p->ctx->alloc, tag);
			if (p->error) {
				p->error->code = GTEXT_YAML_E_OOM;
				p->error->message = "Out of memory creating sequence";
			}
			return GTEXT_YAML_E_OOM;
		}
		
		node->as.sequence.flow_style = flow_style;
		node->as.sequence.count = p->temp.count;
		for (size_t i = 0; i < p->temp.count; i++) {
			node->as.sequence.children[i] = p->temp.items[i];
		}
	} else {
		{
			GTEXT_YAML_Status trailing = mapping_close_trailing_key(p);
			if (trailing != GTEXT_YAML_OK) { gtext_allocator_free(p->ctx->alloc, anchor); gtext_allocator_free(p->ctx->alloc, tag); return trailing; }
		}
		size_t pair_count = p->temp.count / 2;
		node = yaml_node_new_mapping(p->ctx, pair_count, tag, anchor);
		if (!node) {
			gtext_allocator_free(p->ctx->alloc, anchor);
			gtext_allocator_free(p->ctx->alloc, tag);
			if (p->error) {
				p->error->code = GTEXT_YAML_E_OOM;
				p->error->message = "Out of memory creating mapping";
			}
			return GTEXT_YAML_E_OOM;
		}
		
		node->as.mapping.flow_style = flow_style;
		node->as.mapping.count = pair_count;
		for (size_t i = 0; i < pair_count; i++) {
			node->as.mapping.pairs[i].key = p->temp.items[i * 2];
			node->as.mapping.pairs[i].value = p->temp.items[i * 2 + 1];
			node->as.mapping.pairs[i].key_tag = NULL;
			node->as.mapping.pairs[i].value_tag = NULL;
		}
	}

	gtext_allocator_free(p->ctx->alloc, anchor);
	gtext_allocator_free(p->ctx->alloc, tag);

	node_set_source_location(node, source_offset, source_line, source_col);

	if (node->type == GTEXT_YAML_SEQUENCE && node->as.sequence.anchor) {
		GTEXT_YAML_Status anchor_status = register_anchor(p, node->as.sequence.anchor, node);
		if (anchor_status != GTEXT_YAML_OK) {
			return anchor_status;
		}
	}
	if (node->type == GTEXT_YAML_MAPPING && node->as.mapping.anchor) {
		GTEXT_YAML_Status anchor_status = register_anchor(p, node->as.mapping.anchor, node);
		if (anchor_status != GTEXT_YAML_OK) {
			return anchor_status;
		}
	}

	stack_pop(p);

	bool explicit_handled = false;
	GTEXT_YAML_Status explicit_status = capture_explicit_key(p, node, &explicit_handled);
	if (explicit_status != GTEXT_YAML_OK) {
		return explicit_status;
	}
	if (explicit_handled) {
		return GTEXT_YAML_OK;
	}

	if (p->stack.depth == 0) {
		GTEXT_YAML_Status root_status = set_document_root(p, node);
		if (root_status != GTEXT_YAML_OK) return root_status;
	} else {
		GTEXT_YAML_Status supplied =
			block_value_supply_if_missing(
				p, node,
				block_entry_col(p, source_offset, source_col));
		if (supplied != GTEXT_YAML_OK) return supplied;
		if (!temp_add(p, node)) {
			if (p->error) {
				p->error->code = GTEXT_YAML_E_OOM;
				p->error->message = "Out of memory nesting collection";
			}
			return GTEXT_YAML_E_OOM;
		}
		flow_entry_completed(p);
		maybe_finish_block_mapping_value(p);
	}

	return GTEXT_YAML_OK;
}

/**
 * @brief Note that a flow collection now holds a complete entry.
 *
 * The next one needs a "," in front of it.  Within a line two words are one
 * plain scalar, so this only ever fires across a line break: "[a" over "b]"
 * is two entries with nothing between them, which is not a flow sequence.
 */
static void flow_entry_completed(parser_state *p) {
	if (!p || p->stack.depth == 0) return;
	const size_t top = p->stack.depth - 1;
	/* Something landed at this level, so a block sequence's open entry is
	 * no longer empty. */
	p->stack.flow_flags[top] &= (unsigned char)~GTEXT_YAML_BLOCK_ENTRY_OPEN;
	if (p->stack.is_block[top]) return;
	if (p->stack.states[top] == STATE_SEQUENCE) {
		p->stack.flow_flags[top] |= GTEXT_YAML_FLOW_ITEM_DONE;
		return;
	}
	if (p->stack.states[top] == STATE_MAPPING_KEY ||
		p->stack.states[top] == STATE_MAPPING_VALUE) {
		/* A flow mapping's entry is a whole pair, so it is complete only when
		 * the value has landed and the alternating list is even again. */
		if (p->temp.count > 0 && (p->temp.count % 2) == 0) {
			p->stack.flow_flags[top] |= GTEXT_YAML_FLOW_ITEM_DONE;
		}
	}
}

/**
 * @brief Refuse a second entry where the first was never separated from it.
 */
static GTEXT_YAML_Status flow_entry_needs_separator(parser_state *p) {
	if (!p || p->stack.depth == 0) return GTEXT_YAML_OK;
	const size_t top = p->stack.depth - 1;
	if (p->stack.is_block[top]) return GTEXT_YAML_OK;
	if (!(p->stack.flow_flags[top] & GTEXT_YAML_FLOW_ITEM_DONE)) return GTEXT_YAML_OK;

	p->failed = true;
	if (p->error) {
		p->error->code = GTEXT_YAML_E_INVALID;
		p->error->message = "Flow collection entries must be separated by ','";
	}
	return GTEXT_YAML_E_INVALID;
}

/**
 * @brief Close a "[a: 1]" single-pair mapping if one is open.
 *
 * Such a pair is opened by a ":" inside a flow sequence and has no "}" to
 * close it, so the "," or "]" that ends the entry closes it instead.  A
 * pair whose value never arrived gets the null it stands for, the same as
 * anywhere else.
 */
/**
 * @brief Note that the entry an explicit key sat in has just been completed.
 *
 * A "?" key is finished by whatever ends its entry, not only by a ":". In
 * "[ ? a, ? b ]" and "{ ? a, ? b }" the "," ends the first one; a "]" or a
 * "}" would do the same.
 *
 * Leaving the flag set let the next "?" read that key as one still waiting
 * for a value and supply a second null behind the one the entry's end had
 * already put there, so "{ ? a, ? b }" came out as {"a": null, null: "b"}.
 * Inside a flow sequence it was worse: explicit_key_depth pointed at a level
 * about to be popped, so the null went into the sequence and the sequence
 * itself was switched into mapping-key state.
 */
static void explicit_key_entry_done(parser_state *p) {
	if (!p) return;
	if (p->explicit_key_depth != p->stack.depth) return;
	p->explicit_key_active = false;
	p->explicit_key_pending = false;
}

static GTEXT_YAML_Status close_flow_pair(parser_state *p) {
	if (!p || p->stack.depth == 0) return GTEXT_YAML_OK;
	if (!(p->stack.flow_flags[p->stack.depth - 1] & GTEXT_YAML_FLOW_PAIR)) return GTEXT_YAML_OK;

	/* A "?" that never got a key still makes an entry.
	 * ns-flow-map-explicit-entry is
	 *
	 *     ns-flow-map-implicit-entry | ( e-node e-node )
	 *
	 * (7.4), so "[ ? ]" is one pair of empty nodes and comes out as
	 * [{null: null}].  Dropping the key left the pair with no children at
	 * all, which finalized as the empty mapping [{}] - a different document,
	 * and one the grammar has no way to write.
	 *
	 * The level is known to be empty without asking: a pending "?" is one
	 * whose key has not arrived, and the pair was pushed by that same "?",
	 * so nothing has been added to it since. */
	if (p->explicit_key_pending
			&& p->explicit_key_depth == p->stack.depth
			&& !mapping_supply_empty_key(p)) {
		p->failed = true;
		if (p->error) {
			p->error->code = GTEXT_YAML_E_OOM;
			p->error->message = "Out of memory completing flow pair";
		}
		return GTEXT_YAML_E_OOM;
	}

	if ((p->temp.count % 2) == 1 && !mapping_supply_null_value(p)) {
		p->failed = true;
		if (p->error) {
			p->error->code = GTEXT_YAML_E_OOM;
			p->error->message = "Out of memory completing flow pair";
		}
		return GTEXT_YAML_E_OOM;
	}
	explicit_key_entry_done(p);
	return finalize_top_collection(p);
}

/**
 * @brief Refuse a flow collection that the input ended inside.
 *
 * close_block_contexts() deliberately stops at a flow collection, because
 * indentation says nothing about where "[" and "{" end.  Nothing else closed
 * them either, so input ending inside one left the stack standing and the
 * root never set - and a document with a NULL root was handed back as a
 * success.  A caller that checked only for a NULL document got an empty one
 * instead of an error.
 */
/**
 * @brief Refuse a property the stream set aside that no collection claimed.
 *
 * An outer property is held from the scalar that carried it until the block
 * collection that scalar opens is pushed - one event later at most.  Still
 * held means no collection opened, so the two properties named one node:
 *
 *     top2: &node2        &node2 and &v2 are both val2's, and a node
 *       &v2 val2          carries at most one anchor (7.1)
 *
 * Suite case 4JVG.  Checked between events and again when the document ends,
 * because the last node in a document has no event after it - and in
 * gtext_yaml_parse_all() the DOCUMENT_END that would have served never
 * reaches the per-document parser at all.
 */
static GTEXT_YAML_Status check_outer_property_claimed(parser_state *p) {
	if (!p || (!p->outer_anchor && !p->outer_tag)) return GTEXT_YAML_OK;
	const bool anchors = p->outer_anchor != NULL;
	p->failed = true;
	if (p->error) {
		p->error->code = GTEXT_YAML_E_INVALID;
		if (p->outer_from_alias) {
			p->error->message = anchors
				? "An alias node may not carry an anchor"
				: "An alias node may not carry a tag";
		} else {
			p->error->message = anchors
				? "Node has more than one anchor"
				: "Node has more than one tag";
		}
	}
	return GTEXT_YAML_E_INVALID;
}

static GTEXT_YAML_Status check_flow_contexts_closed(parser_state *p) {
	if (!p || p->stack.depth == 0) return GTEXT_YAML_OK;
	if (p->stack.is_block[p->stack.depth - 1]) return GTEXT_YAML_OK;

	p->failed = true;
	if (p->error && p->error->code == GTEXT_YAML_OK) {
		p->error->code = GTEXT_YAML_E_INVALID;
		p->error->message = "Unterminated flow collection";
	}
	return GTEXT_YAML_E_INVALID;
}

/**
 * @brief Close the block collections a line at @p new_indent has left.
 *
 * A collection indented further than the new line has ended.  A block
 * sequence at exactly the new line's indentation has ended too, because YAML
 * lets a sequence sit at the same column as the key that owns it:
 *
 *     a:
 *     - 1
 *     b: 2
 *
 * Here "b" is a key of the mapping that owns "a", not a third entry of the
 * sequence.  Only another entry keeps that sequence open, so @p starts_entry
 * says whether this line begins with a "-" at that column.  A block *mapping*
 * at the same indentation is never closed: that is the mapping the new key
 * belongs to.
 */
static GTEXT_YAML_Status close_block_contexts_for(
	parser_state *p,
	int new_indent,
	bool starts_entry
) {
	if (!p) return GTEXT_YAML_E_INVALID;

	while (p->stack.depth > 0) {
		if (!stack_top_is_block(p)) break;
		const int top_indent = stack_top_indent(p);
		if (new_indent < top_indent) {
			/* left the collection entirely */
		} else if (new_indent == top_indent && !starts_entry &&
			p->stack.states[p->stack.depth - 1] == STATE_SEQUENCE) {
			/* a sequence at the owning key's own column, and this is not
			   another entry of it */
		} else {
			break;
		}

		GTEXT_YAML_Status status = finalize_top_collection(p);
		if (status != GTEXT_YAML_OK) return status;
	}

	return GTEXT_YAML_OK;
}

static GTEXT_YAML_Status close_block_contexts(parser_state *p, int new_indent) {
	return close_block_contexts_for(p, new_indent, false);
}

/**
 * @brief Was a node's properties written left of a collection already open?
 *
 * c-ns-properties reaches a block collection only through s-separate(n+1,c)
 * (8.2), so an anchor or tag has to be indented past every block collection
 * that was already open when it was written.  "&node" alone on a line between
 * two "- " entries is at the sequence's own indentation: it introduces
 * nothing, and there is no node for it to name.
 *
 * The column of the node the properties end up attached to will not do for
 * this.  By the time the scalar arrives, the collection the properties
 * introduced has been opened at a deeper indentation, and the scalar sits
 * inside it.  GTEXT_YAML_Event carries the properties' own position instead.
 *
 * Levels that opened on the properties' line or later are exactly the ones the
 * properties introduce, so they are skipped; the first level below those is
 * the one the properties had to clear.
 *
 * Flow levels need no case of their own.  Indentation constrains nothing
 * inside "[" or "{", and every flow level is pushed with an indent of -1, so
 * the comparison below is already false for them.
 */
static bool property_left_of_open_collection(
	const parser_state *p,
	const GTEXT_YAML_Event *event
) {
	if (!event || event->prop_col < 0) return false;
	/* Properties written on the same line as the node they name are a key's
	   own properties, and a key sits at its mapping's indentation, not past
	   it: "!!str 23: !!bool false" is a well-formed entry of a mapping at
	   column 0 (suite case 74H7).  An implicit key and its properties have
	   to share a line, so a property left on a line of its own is never one of
	   these - that is what this rule is about. */
	if (event->prop_line == event->line) return false;
	for (size_t i = p->stack.depth; i > 0; i--) {
		const size_t idx = i - 1;
		if (p->stack.temps[idx].source_line >= event->prop_line) continue;
		return event->prop_col <= p->stack.indents[idx];
	}
	return false;
}

/**
 * @brief Streaming parser callback - builds DOM from events.
 */
/**
 * @brief Take the scanner's complaint off a stream before the stream is freed.
 *
 * Returns a zeroed error when the scanner never refused a token, which reads
 * as "nothing to say" to parse_error_fallback() below.
 */
static GTEXT_YAML_Error stream_take_error(GTEXT_YAML_Stream *stream) {
	GTEXT_YAML_Error e;
	memset(&e, 0, sizeof(e));
	if (stream) gtext_yaml_stream_last_error(stream, &e);
	return e;
}

/**
 * @brief Describe a parse failure that nothing more specific has described.
 *
 * A fault the scanner caught arrives here as a bare status, because the token
 * loops in stream.c hand back nothing else; the scanner's own message says
 * which fault it was and where, and is far better than the "Parse error" this
 * used to report for every one of them. Leaves @p error alone when it already
 * holds a failure - whoever set it was closer to the problem.
 */
static void parse_error_fallback(
	GTEXT_YAML_Error *error,
	GTEXT_YAML_Status status,
	const GTEXT_YAML_Error *scan_err
) {
	if (!error || error->code != GTEXT_YAML_OK) return;
	if (scan_err && scan_err->code != GTEXT_YAML_OK && scan_err->message) {
		error->code = scan_err->code;
		error->message = scan_err->message;
		error->offset = scan_err->offset;
		error->line = scan_err->line;
		error->col = scan_err->col;
		return;
	}
	error->code = status;
	error->message = "Parse error";
}

static GTEXT_YAML_Status parse_callback(
	GTEXT_YAML_Stream *s,
	const void *event_payload,
	void *user_data
) {
	parser_state *p = (parser_state *)user_data;
	if (p->failed) return GTEXT_YAML_E_STATE;

	/* Point the context at the text this event's offset indexes, which is the
	   scanner's decoded stream and not the bytes the caller handed in. The
	   two differ for a byte order mark and for every encoding but UTF-8, and
	   the helpers below that ask "what stands between here and the start of
	   the line?" were reading the caller's buffer at a decoded offset - so
	   "a: 1" over "b: 2" was refused outright in UTF-16, and in UTF-8 with a
	   mark in front of it.

	   Refreshed here rather than kept, because the scanner owns the buffer
	   and moves it when it grows. Every path that builds a DOM arrives
	   through this function, so this is the one place it needs saying. */
	{
		const char *decoded = NULL;
		size_t decoded_len = 0;
		if (gtext_yaml_stream_decoded_input(s, &decoded, &decoded_len)) {
			yaml_context_set_decoded_input(p->ctx, decoded, decoded_len);
		}
	}

	const GTEXT_YAML_Event *event = (const GTEXT_YAML_Event *)event_payload;
	GTEXT_YAML_Event_Type type = event->type;

	if (event->line >= 0 && event->line != p->last_event_line) {
		const bool starts_entry = (type == GTEXT_YAML_EVENT_INDICATOR
			&& event->data.indicator == '-');
		GTEXT_YAML_Status close_status =
			close_block_contexts_for(p, event_node_col(event), starts_entry);
		if (close_status != GTEXT_YAML_OK) return close_status;
		p->last_event_line = event->line;
	}
	
	/* Skip events if first document already complete (multi-doc streams) */
	if (p->first_document_complete) {
		return GTEXT_YAML_OK;
	}

	if (property_left_of_open_collection(p, event)) {
		p->failed = true;
		if (p->error) {
			p->error->code = GTEXT_YAML_E_INVALID;
			p->error->message =
				"Anchor or tag not indented past the collection it is written in";
		}
		return GTEXT_YAML_E_INVALID;
	}

	/* A directive prologue is a document only when a "---" follows it
	 * (l-directive-document, 9.2).  The marker arrives as an explicit
	 * DOCUMENT_START; a bare document emits no DOCUMENT_START at all, so the
	 * test has to be here, where content first appears.  Without it the
	 * directives were simply applied to whatever came next and "%YAML 1.2"
	 * over a bare "v" parsed as a document the grammar does not admit. */
	if (p->directive_awaits_marker) {
		switch (type) {
			case GTEXT_YAML_EVENT_DOCUMENT_START:
				if (event->explicit_marker) p->directive_awaits_marker = false;
				break;
			case GTEXT_YAML_EVENT_DOCUMENT_END:
				break;
			case GTEXT_YAML_EVENT_SCALAR:
			case GTEXT_YAML_EVENT_SEQUENCE_START:
			case GTEXT_YAML_EVENT_MAPPING_START:
			case GTEXT_YAML_EVENT_ALIAS:
			case GTEXT_YAML_EVENT_INDICATOR:
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_INVALID;
					p->error->message = "Directives must be followed by '---'";
				}
				return GTEXT_YAML_E_INVALID;
			default:
				break;
		}
	}

	/* An outer property is held from the node that carried it until the
	 * block collection that node opens is pushed, which happens either
	 * inside that same scalar event (a sequence, whose "-" pushed the level
	 * before the scalar arrived) or in the ":" that follows it (a mapping).
	 * Anything else reaching here with one still held means no collection
	 * opened, so the two properties named the one node.
	 *
	 * Unless the node the property precedes is a flow collection, and then
	 * the ":" cannot arrive until the collection is closed:
	 *
	 *     &mapping                      &mapping waits through five events
	 *     &key [ &item a, b, c ]: value      before the ":" claims it
	 *
	 * Every one of those is inside the flow level the property opened, so
	 * the level itself says the wait is still legitimate (suite case 6BFJ).
	 * A flow collection that no ":" follows still fails the check, one event
	 * after the "]" pops the level. */
	const bool inside_flow = p->stack.depth > 0
		&& !p->stack.is_block[p->stack.depth - 1];
	if (!inside_flow && !(type == GTEXT_YAML_EVENT_INDICATOR
			&& event->data.indicator == ':')) {
		GTEXT_YAML_Status held = check_outer_property_claimed(p);
		if (held != GTEXT_YAML_OK) return held;
	}

	switch (type) {
		case GTEXT_YAML_EVENT_STREAM_START:
			/* Start of stream - nothing to do */
			break;
			
		case GTEXT_YAML_EVENT_DOCUMENT_START:
			/* Start of a document */
			if (!p->document_started) {
				p->document_started = true;
				if (p->doc) p->doc->explicit_start = event->explicit_marker;
				/* This is the first (or only) document, parse it */
			} else {
				/* We've already started a document, this is a second one */
				/* Stop parsing - we only want the first document */
				p->first_document_complete = true;
			}
			break;

		case GTEXT_YAML_EVENT_DIRECTIVE: {
			const char *name = event->data.directive.name;
			const char *value = event->data.directive.value;
			const char *value2 = event->data.directive.value2;

			if (!name) {
				break;
			}

			/* Directives belong to the prologue of a document: they may only
			 * follow the start of the stream or a "..." that closed the one
			 * before (9.2, l-directive-document). One arriving after content
			 * has no document to prologue, and was simply being applied to
			 * the document already underway. */
			if (p->root || p->temp.count > 0 || p->stack.depth > 0) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_INVALID;
					p->error->message =
						"Directive after content, with no '...' to close the document";
				}
				return GTEXT_YAML_E_INVALID;
			}

			p->doc->has_directives = true;
			p->directive_awaits_marker = true;
			if (strcmp(name, "YAML") == 0) {
				/* "%YAML" takes exactly one parameter, the version (6.8.1),
				 * and a document may carry at most one of them. Extra words
				 * were ignored and a second directive silently replaced the
				 * first. */
				if (value2) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message = "YAML directive takes only a version";
					}
					return GTEXT_YAML_E_INVALID;
				}
				if (p->seen_yaml_directive) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message = "Repeated YAML directive";
					}
					return GTEXT_YAML_E_INVALID;
				}
				p->seen_yaml_directive = true;
				if (!value) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message = "YAML directive missing version";
					}
					return GTEXT_YAML_E_INVALID;
				}
				char *end = NULL;
				long major = strtol(value, &end, 10);
				if (!end || *end != '.') {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message = "Invalid YAML directive version";
					}
					return GTEXT_YAML_E_INVALID;
				}
				long minor = strtol(end + 1, &end, 10);
				if (!end || *end != '\0') {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message = "Invalid YAML directive version";
					}
					return GTEXT_YAML_E_INVALID;
				}
				/* 6.8.1: a processor must refuse a document whose major
				 * version it does not implement, and should carry on with a
				 * warning when only the minor version is ahead of it - the
				 * minor versions are meant to stay compatible. Neither was
				 * done, so "%YAML 2.0" parsed as though it said 1.2. */
				if (major != 1) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message =
							"YAML directive names a major version this parser does not implement";
					}
					return GTEXT_YAML_E_INVALID;
				}
				if (minor > 2) {
					GTEXT_YAML_Status warn = gtext_yaml_emit_warning(
						p->doc ? &p->doc->options : NULL,
						GTEXT_YAML_WARNING_YAML_VERSION,
						"YAML directive names a minor version newer than 1.2",
						p->error
					);
					if (warn != GTEXT_YAML_OK) {
						p->failed = true;
						return warn;
					}
				}
				p->doc->yaml_version_major = (int)major;
				p->doc->yaml_version_minor = (int)minor;
			} else if (strcmp(name, "TAG") == 0) {
				if (!value || !value2) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message = "TAG directive missing handle or prefix";
					}
					return GTEXT_YAML_E_INVALID;
				}
				/* 6.8.2: at most one "%TAG" per handle per document. The
				 * table simply replaced the prefix, so the second directive
				 * quietly won and every shorthand written before it in the
				 * same prologue resolved against a prefix the author had
				 * already superseded. */
				if (tag_handle_lookup(p, value)) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message =
							"Repeated TAG directive for one handle";
					}
					return GTEXT_YAML_E_INVALID;
				}
				if (!tag_handle_add(p, value, value2)) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_OOM;
						p->error->message = "Out of memory storing tag handle";
					}
					return GTEXT_YAML_E_OOM;
				}
			}
			break;
		}
		case GTEXT_YAML_EVENT_COMMENT: {
			const char *comment = event->data.comment.ptr;
			bool inline_comment = event->data.comment.inline_comment;
			if (!comment) {
				break;
			}
			if (inline_comment && p->last_emitted_node &&
				p->last_emitted_line == event->line) {
				parser_attach_inline_comment(p, p->last_emitted_node, comment);
			} else {
				size_t existing = p->pending_leading_comment
					? strlen(p->pending_leading_comment)
					: 0;
				size_t add_len = strlen(comment);
				size_t extra = existing > 0 ? 1 : 0;
				char *buf = (char *)gtext_allocator_malloc(p->ctx->alloc, existing + add_len + extra + 1);
				if (!buf) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_OOM;
						p->error->message = "Out of memory storing comment";
					}
					return GTEXT_YAML_E_OOM;
				}
				if (existing > 0) {
					memcpy(buf, p->pending_leading_comment, existing);
					buf[existing] = '\n';
					memcpy(buf + existing + 1, comment, add_len);
					buf[existing + 1 + add_len] = '\0';
					gtext_allocator_free(p->ctx->alloc, p->pending_leading_comment);
				} else {
					memcpy(buf, comment, add_len);
					buf[add_len] = '\0';
				}
				p->pending_leading_comment = buf;
			}
			break;
		}
			
		case GTEXT_YAML_EVENT_SCALAR: {
			/* Create scalar node */
			GTEXT_YAML_Node *node = yaml_node_new_scalar(
				p->ctx,
				event->data.scalar.ptr,
				event->data.scalar.len,
				event->tag,     /* Tag from event (may be NULL) */
				event->anchor   /* Anchor from event (may be NULL) */
			);
			
			if (!node) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_OOM;
					p->error->message = "Out of memory creating scalar node";
				}
				return GTEXT_YAML_E_OOM;
			}

			node_set_source_location(node, event->offset, event->line, event->col);
			node->as.scalar.scalar_style = event->scalar_style;

			/* Set for every scalar, so a scalar without one clears whatever the
			 * previous scalar left behind. */
			p->last_scalar_tag_own_line = (event->tag != NULL
				&& event->tag_line > 0 && event->tag_line < event->line);
			p->last_scalar_anchor_own_line = (event->anchor != NULL
				&& event->anchor_line > 0 && event->anchor_line < event->line);

			{
				GTEXT_YAML_Status held = parser_hold_outer_props(p, event);
				if (held != GTEXT_YAML_OK) return held;
			}

			parser_attach_leading_comment(p, node);
			
			/* Register anchor if present */
			if (event->anchor) {
				GTEXT_YAML_Status anchor_status = register_anchor(p, event->anchor, node);
				if (anchor_status != GTEXT_YAML_OK) {
					return anchor_status;
				}
			}

			bool explicit_handled = false;
			GTEXT_YAML_Status explicit_status = capture_explicit_key(p, node, &explicit_handled);
			if (explicit_status != GTEXT_YAML_OK) {
				return explicit_status;
			}
			if (explicit_handled) {
				p->last_scalar_node = node;
				p->last_scalar_line = event->line;
				p->last_scalar_col = event->col;
				p->last_scalar_offset = event->offset;
				p->last_scalar_key_col = line_key_col_from_offset(p, event->offset);
				p->last_emitted_node = node;
				p->last_emitted_line = event->line;
				break;
			}
			
			/* Add to parent or set as root */
			if (p->stack.depth == 0) {
				GTEXT_YAML_Status root_status = set_document_root(p, node);
				if (root_status != GTEXT_YAML_OK) return root_status;
				p->last_scalar_in_root = true;
				p->last_scalar_in_temp = false;
			} else {
				/* A scalar at the key's own column, while a value is still
				 * expected, is the next key rather than that value.  Supply
				 * the null the absent value stands for, before this scalar
				 * takes its place in the alternating list. */
				if (block_value_is_missing(p, event_node_col(event))) {
					if (!mapping_supply_null_value(p)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_OOM;
							p->error->message = "Out of memory completing mapping value";
						}
						return GTEXT_YAML_E_OOM;
					}
					p->stack.states[p->stack.depth - 1] = STATE_MAPPING_KEY;
				}

				/* Before the add, so that "was this the first item?" is still
				 * answerable. */
				const bool first_in_level = (p->temp.count == 0);

				{
					GTEXT_YAML_Status sep_status = flow_entry_needs_separator(p);
					if (sep_status != GTEXT_YAML_OK) return sep_status;
				}

				/* A scalar indented past its block mapping is that mapping's
				 * value, and only when a key above is still waiting for one.
				 * With every key already paired there is nothing for it to be,
				 * and it used to become a trailing key with a null value - so
				 * "a: |" over a deeper "deep" over a shallower "shallow" gave
				 * {"a": "deep\n", "shallow": null} for input no other parser
				 * accepts.  A "-" never reaches here, so a block sequence at
				 * its key's own column is unaffected.
				 *
				 * The column is the line's first non-space, not the scalar's
				 * own: a tag or anchor sits before the scalar and belongs to
				 * the same node, so "!!str true" as a key starts where the
				 * tag does. */
				const int node_col = line_key_col_from_offset(p, event->offset);
				if (node_col >= 0 &&
					p->stack.is_block[p->stack.depth - 1] &&
					(p->stack.states[p->stack.depth - 1] == STATE_MAPPING_KEY ||
					 p->stack.states[p->stack.depth - 1] == STATE_MAPPING_VALUE) &&
					p->stack.indents[p->stack.depth - 1] >= 0 &&
					node_col > p->stack.indents[p->stack.depth - 1] &&
					(p->temp.count % 2) == 0) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_INVALID;
						p->error->message =
							"Scalar indented deeper than its mapping with no key to hold it";
					}
					return GTEXT_YAML_E_INVALID;
				}

				if (!temp_add(p, node)) {
					p->failed = true;
					if (p->error) {
						p->error->code = GTEXT_YAML_E_OOM;
						p->error->message = "Out of memory adding child node";
					}
					return GTEXT_YAML_E_OOM;
				}

				/* An own-line tag before a block sequence reaches the parser on
				 * the sequence's first item, because the '-' that opens the
				 * sequence carries no node of its own.  It belongs to the
				 * sequence. */
				if (first_in_level
					&& p->stack.states[p->stack.depth - 1] == STATE_SEQUENCE
					&& p->stack.is_block[p->stack.depth - 1]) {
					GTEXT_YAML_Status adopted = adopt_own_line_tag(p, node);
					if (adopted == GTEXT_YAML_OK) {
						adopted = adopt_own_line_anchor(p, node);
					}
					if (adopted != GTEXT_YAML_OK) return adopted;
				}

				p->last_scalar_in_root = false;
				p->last_scalar_in_temp = true;
				p->last_scalar_temp_depth = p->stack.depth;

				flow_entry_completed(p);
				maybe_finish_block_mapping_value(p);
			}

			p->last_scalar_node = node;
			p->last_scalar_line = event->line;
			p->last_scalar_col = event->col;
			p->last_scalar_offset = event->offset;
			p->last_scalar_key_col = line_key_col_from_offset(p, event->offset);
			p->last_emitted_node = node;
			p->last_emitted_line = event->line;
			break;
		}
		
		case GTEXT_YAML_EVENT_SEQUENCE_START: {
			/* Start building a sequence - we don't know the size yet */
			const GTEXT_YAML_Event *evt = (const GTEXT_YAML_Event *)event;

			{
				GTEXT_YAML_Status held = parser_hold_outer_props(p, evt);
				if (held != GTEXT_YAML_OK) return held;
			}

			/* Checked as this one opens, not as it closes: by the time it
			 * closes its own level is on the stack and the entry it has to be
			 * separated from is a level below. */
			{
				GTEXT_YAML_Status sep_status = flow_entry_needs_separator(p);
				if (sep_status != GTEXT_YAML_OK) return sep_status;
			}
			
			/* Push placeholder (we'll create the actual node on SEQUENCE_END) */
			/* Store anchor and tag from event for later use */
			if (!stack_push(
				p,
				NULL,
				STATE_SEQUENCE,
				evt->anchor,
				evt->tag,
				-1,
				false,
				evt->offset,
				evt->line,
				evt->col
			)) {
				return stack_push_failed(p, "Out of memory tracking sequence");
			}
			break;
		}
		
		case GTEXT_YAML_EVENT_SEQUENCE_END: {
			/* A "[a: 1]" pair has no "}" of its own; the "]" that ends the
			 * entry closes it. */
			{
				GTEXT_YAML_Status pair_status = close_flow_pair(p);
				if (pair_status != GTEXT_YAML_OK) return pair_status;
			}
			/* A "]" closes the flow sequence a "[" opened, and there has to be
			 * one of those.  Nothing asked, so the children of whatever
			 * collection happened to be open were handed to
			 * yaml_node_new_sequence() and came back as a sequence:
			 *
			 *     a: 1        was  ["a", 1]      - a mapping flattened
			 *     ]
			 *
			 *     {a: 1]      was  ["a", 1]      - mismatched, and reshaped
			 *     ]           was  []            - a bracket closing nothing
			 *
			 * Every one of those is a different document from the one that
			 * was written, produced without a word. */
			if (p->stack.depth == 0
					|| p->stack.is_block[p->stack.depth - 1]
					|| p->stack.states[p->stack.depth - 1] != STATE_SEQUENCE) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_INVALID;
					p->error->message = "Unexpected ] without matching [";
				}
				return GTEXT_YAML_E_INVALID;
			}
			/* Get anchor and tag from saved stack state */
			char *anchor = NULL;
			char *tag = NULL;
			size_t source_offset = 0;
			int source_line = 0;
			int source_col = 0;
			stack_get_and_clear_metadata(
				p,
				&anchor,
				&tag,
				&source_offset,
				&source_line,
				&source_col
			);
			
			/* Create sequence node with collected children */
			GTEXT_YAML_Node *node = yaml_node_new_sequence(
				p->ctx,
				p->temp.count,
				tag,
				anchor
			);
			
			/* Free the malloc'd anchor and tag strings */
			gtext_allocator_free(p->ctx->alloc, anchor);
			gtext_allocator_free(p->ctx->alloc, tag);
			
			if (!node) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_OOM;
					p->error->message = "Out of memory creating sequence node";
				}
				return GTEXT_YAML_E_OOM;
			}

			parser_attach_leading_comment(p, node);
			
			/* Only "]" reaches here, and it closes a flow sequence. */
			node->as.sequence.flow_style = GTEXT_YAML_FLOW_STYLE_FLOW;
			/* Copy children into node */
			for (size_t i = 0; i < p->temp.count; i++) {
				node->as.sequence.children[i] = p->temp.items[i];
			}
			node->as.sequence.count = p->temp.count;
			node_set_source_location(node, source_offset, source_line, source_col);
			
			/* Register anchor if present */
			if (node->as.sequence.anchor) {
				GTEXT_YAML_Status anchor_status = register_anchor(p, node->as.sequence.anchor, node);
				if (anchor_status != GTEXT_YAML_OK) {
					return anchor_status;
				}
			}
			
			/* Pop sequence from stack (restores parent temp) */
			stack_pop(p);
			
			/* Add to parent or set as root */
			if (p->stack.depth == 0) {
				GTEXT_YAML_Status root_status = set_document_root(p, node);
				if (root_status != GTEXT_YAML_OK) return root_status;
			} else {
				bool explicit_handled = false;
				GTEXT_YAML_Status explicit_status = capture_explicit_key(p, node, &explicit_handled);
				if (explicit_status != GTEXT_YAML_OK) {
					return explicit_status;
				}
				if (!explicit_handled) {
					GTEXT_YAML_Status supplied =
						block_value_supply_if_missing(
				p, node,
				block_entry_col(p, source_offset, source_col));
					if (supplied != GTEXT_YAML_OK) return supplied;
					if (!temp_add(p, node)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_OOM;
							p->error->message = "Out of memory nesting sequence";
						}
						return GTEXT_YAML_E_OOM;
					}
					flow_entry_completed(p);
					maybe_finish_block_mapping_value(p);
				}
			}

			p->last_emitted_node = node;
			p->last_emitted_line = event->line;
			
			break;
		}
		
		case GTEXT_YAML_EVENT_MAPPING_START: {
			/* Start building a mapping */
			const GTEXT_YAML_Event *evt = (const GTEXT_YAML_Event *)event;

			{
				GTEXT_YAML_Status held = parser_hold_outer_props(p, evt);
				if (held != GTEXT_YAML_OK) return held;
			}

			{
				GTEXT_YAML_Status sep_status = flow_entry_needs_separator(p);
				if (sep_status != GTEXT_YAML_OK) return sep_status;
			}
			
			if (!stack_push(
				p,
				NULL,
				STATE_MAPPING_KEY,
				evt->anchor,
				evt->tag,
				-1,
				false,
				evt->offset,
				evt->line,
				evt->col
			)) {
				return stack_push_failed(p, "Out of memory tracking mapping");
			}
			break;
		}
		
		case GTEXT_YAML_EVENT_MAPPING_END: {
			/* The same question for "}": it closes the flow mapping a "{"
			 * opened.  Without asking, "[1, 2}" paired the sequence's entries
			 * and came back as {1: 2}, and a "}" with nothing open at all
			 * produced the empty mapping.
			 *
			 * A single-pair level is not a flow mapping either.  It was
			 * opened by a ":" or a "?" inside a flow sequence and is closed
			 * by whatever ends that entry, so a "}" cannot be the thing that
			 * ends it: "[a: 1}" is mismatched however the pair is read. */
			if (p->stack.depth == 0
					|| p->stack.is_block[p->stack.depth - 1]
					|| (p->stack.flow_flags[p->stack.depth - 1]
						& GTEXT_YAML_FLOW_PAIR)
					|| (p->stack.states[p->stack.depth - 1] != STATE_MAPPING_KEY
						&& p->stack.states[p->stack.depth - 1]
							!= STATE_MAPPING_VALUE)) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_INVALID;
					p->error->message = "Unexpected } without matching {";
				}
				return GTEXT_YAML_E_INVALID;
			}
			/* Get anchor and tag from saved stack state */
			char *anchor = NULL;
			char *tag = NULL;
			size_t source_offset = 0;
			int source_line = 0;
			int source_col = 0;
			stack_get_and_clear_metadata(
				p,
				&anchor,
				&tag,
				&source_offset,
				&source_line,
				&source_col
			);
			
			/* Create mapping node with collected key-value pairs */
			/* temp.items should have [key0, val0, key1, val1, ...] */
			{
				GTEXT_YAML_Status trailing = mapping_close_trailing_key(p);
				if (trailing != GTEXT_YAML_OK) { gtext_allocator_free(p->ctx->alloc, anchor); gtext_allocator_free(p->ctx->alloc, tag); return trailing; }
			}
			size_t pair_count = p->temp.count / 2;
			
			GTEXT_YAML_Node *node = yaml_node_new_mapping(
				p->ctx,
				pair_count,
				tag,
				anchor
			);
			
			/* Free the malloc'd anchor and tag strings */
			gtext_allocator_free(p->ctx->alloc, anchor);
			gtext_allocator_free(p->ctx->alloc, tag);
			
			if (!node) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_OOM;
					p->error->message = "Out of memory creating mapping node";
				}
				return GTEXT_YAML_E_OOM;
			}

			parser_attach_leading_comment(p, node);
			
			/* Only "}" reaches here, and it closes a flow mapping. */
			node->as.mapping.flow_style = GTEXT_YAML_FLOW_STYLE_FLOW;
			/* Copy pairs into node */
			for (size_t i = 0; i < pair_count; i++) {
				node->as.mapping.pairs[i].key = p->temp.items[i * 2];
				node->as.mapping.pairs[i].value = p->temp.items[i * 2 + 1];
				node->as.mapping.pairs[i].key_tag = NULL;
				node->as.mapping.pairs[i].value_tag = NULL;
			}
			node->as.mapping.count = pair_count;
			node_set_source_location(node, source_offset, source_line, source_col);
			
			/* Register anchor if present */
			if (node->as.mapping.anchor) {
				GTEXT_YAML_Status anchor_status = register_anchor(p, node->as.mapping.anchor, node);
				if (anchor_status != GTEXT_YAML_OK) {
					return anchor_status;
				}
			}
			
			/* Pop mapping from stack (restores parent temp) */
			stack_pop(p);
			
			/* Add to parent or set as root */
			if (p->stack.depth == 0) {
				GTEXT_YAML_Status root_status = set_document_root(p, node);
				if (root_status != GTEXT_YAML_OK) return root_status;
			} else {
				bool explicit_handled = false;
				GTEXT_YAML_Status explicit_status = capture_explicit_key(p, node, &explicit_handled);
				if (explicit_status != GTEXT_YAML_OK) {
					return explicit_status;
				}
				if (!explicit_handled) {
					GTEXT_YAML_Status supplied =
						block_value_supply_if_missing(
				p, node,
				block_entry_col(p, source_offset, source_col));
					if (supplied != GTEXT_YAML_OK) return supplied;
					if (!temp_add(p, node)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_OOM;
							p->error->message = "Out of memory nesting mapping";
						}
						return GTEXT_YAML_E_OOM;
					}
					flow_entry_completed(p);
					maybe_finish_block_mapping_value(p);
				}
			}

			p->last_emitted_node = node;
			p->last_emitted_line = event->line;
			
			break;
		}
		
		case GTEXT_YAML_EVENT_STREAM_END:
			/* End of stream */
			break;
			
		case GTEXT_YAML_EVENT_DOCUMENT_END:
			/* End of document */
			if (p->document_started && !p->first_document_complete) {
				GTEXT_YAML_Status close_status = close_block_contexts(p, -1);
				if (close_status != GTEXT_YAML_OK) return close_status;
				
				if (p->doc) p->doc->explicit_end = event->explicit_marker;
				/* First document is complete */
				p->first_document_complete = true;
				p->document_count = 1;
			}
			break;
			
		case GTEXT_YAML_EVENT_ALIAS: {
			/* Create alias node */
			const GTEXT_YAML_Parse_Options *opts = p->doc ? &p->doc->options : NULL;

			{
				GTEXT_YAML_Status held = parser_hold_outer_props(p, event);
				if (held != GTEXT_YAML_OK) return held;
			}
			const char *anchor_name = event->data.alias_name;
			GTEXT_YAML_Node *node = NULL;
			bool explicit_handled = false;
			GTEXT_YAML_Status explicit_status = GTEXT_YAML_OK;

			if (opts && !opts->allow_aliases) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_INVALID;
					p->error->message = "Aliases are disabled by parse options";
				}
				return GTEXT_YAML_E_INVALID;
			}

			if (!anchor_name) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_INVALID;
					p->error->message = "Alias event missing anchor name";
				}
				return GTEXT_YAML_E_INVALID;
			}
			
			node = yaml_node_new_alias(p->ctx, anchor_name);
			if (!node) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_OOM;
					p->error->message = "Out of memory creating alias node";
				}
				return GTEXT_YAML_E_OOM;
			}

			node_set_source_location(node, event->offset, event->line, event->col);

			parser_attach_leading_comment(p, node);
			
			/* Track alias for later resolution */
			/* Bound here rather than after the parse: an anchor may be
			 * redefined, and this alias means whichever node held the name
			 * when it was written. */
			if (!track_alias(p, node, lookup_anchor(p, anchor_name),
					anchor_open_on_stack(p, anchor_name))) {
				p->failed = true;
				if (p->error) {
					p->error->code = GTEXT_YAML_E_OOM;
					p->error->message = "Out of memory tracking alias";
				}
				return GTEXT_YAML_E_OOM;
			}
			
			/* Add to parent or set as root */
			if (p->stack.depth == 0) {
				GTEXT_YAML_Status root_status = set_document_root(p, node);
				if (root_status != GTEXT_YAML_OK) return root_status;
				p->last_scalar_in_root = true;
				p->last_scalar_in_temp = false;
			} else {
				explicit_status = capture_explicit_key(p, node, &explicit_handled);
				if (explicit_status != GTEXT_YAML_OK) {
					return explicit_status;
				}
				if (!explicit_handled) {
					/* An alias at the key's own column, while a value is
					 * still expected, is the next key rather than that
					 * value - the same rule the scalar case has had for as
					 * long as the block mapping has, and an alias may stand
					 * wherever a scalar does (7.1).  Only scalars and flow
					 * collections were ever asked, so a key spelled as an
					 * alias and left without a value never produced the null
					 * it stands for: the alternating list stayed one short,
					 * and the *next* entry's ":" then found an even-length
					 * list in front of it and reported a key missing from a
					 * line that had one.  Two aliases were needed to see it:
					 * with a scalar in the second entry, the SCALAR event's
					 * own copy of this rule supplied the value the alias had
					 * not, and the document parsed. */
					if (block_value_is_missing(p, alias_node_col(event))) {
						if (!mapping_supply_null_value(p)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message =
									"Out of memory completing mapping value";
							}
							return GTEXT_YAML_E_OOM;
						}
						p->stack.states[p->stack.depth - 1] = STATE_MAPPING_KEY;
					}
					GTEXT_YAML_Status sep_status = flow_entry_needs_separator(p);
					if (sep_status != GTEXT_YAML_OK) return sep_status;
					if (!temp_add(p, node)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_OOM;
							p->error->message = "Out of memory adding alias node";
						}
						return GTEXT_YAML_E_OOM;
					}
					p->last_scalar_in_root = false;
					p->last_scalar_in_temp = true;
					p->last_scalar_temp_depth = p->stack.depth;
					flow_entry_completed(p);
					maybe_finish_block_mapping_value(p);
				}
			}

			/* An alias may stand where a key does - "*b : *a" is a mapping
			 * whose key is whatever &b named (7.1) - so the ":" that follows
			 * has to find it the same way it finds a scalar. Without this it
			 * looked for a key on an earlier line and refused the document. */
			if (!explicit_handled) {
				p->last_scalar_node = node;
				p->last_scalar_line = event->line;
				p->last_scalar_col = event->col;
				p->last_scalar_offset = event->offset;
				p->last_scalar_key_col = line_key_col_from_offset(p, event->offset);
			}

			p->last_emitted_node = node;
			p->last_emitted_line = event->line;
			break;
		}
			
		case GTEXT_YAML_EVENT_INDICATOR: {
			/* Handle structural indicators: [ ] { } , : - */
			char ch = event->data.indicator;
			
			switch (ch) {
				case '[':
					/* Start flow sequence (fallback if START event not emitted) */
					{
						GTEXT_YAML_Status sep_status = flow_entry_needs_separator(p);
						if (sep_status != GTEXT_YAML_OK) return sep_status;
					}
					if (!stack_push(
						p,
						NULL,
						STATE_SEQUENCE,
						NULL,
						NULL,
						-1,
						false,
						event->offset,
						event->line,
						event->col
					)) {
						return stack_push_failed(p, "Out of memory tracking sequence");
					}
					break;
					
				case ']': {
					/* End flow sequence - create node with collected items */
					{
						GTEXT_YAML_Status pair_status = close_flow_pair(p);
						if (pair_status != GTEXT_YAML_OK) return pair_status;
					}
					if (p->stack.depth == 0 || p->stack.states[p->stack.depth - 1] != STATE_SEQUENCE) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message = "Unexpected ] without matching [";
						}
						return GTEXT_YAML_E_INVALID;
					}
					char *anchor = NULL;
					char *tag = NULL;
					size_t source_offset = 0;
					int source_line = 0;
					int source_col = 0;
					stack_get_and_clear_metadata(
						p,
						&anchor,
						&tag,
						&source_offset,
						&source_line,
						&source_col
					);
					
					GTEXT_YAML_Node *node = yaml_node_new_sequence(
						p->ctx,
						p->temp.count,
						tag,
						anchor
					);
					gtext_allocator_free(p->ctx->alloc, anchor);
					gtext_allocator_free(p->ctx->alloc, tag);
					if (!node) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_OOM;
							p->error->message = "Out of memory creating sequence";
						}
						return GTEXT_YAML_E_OOM;
					}

					parser_attach_leading_comment(p, node);
					node->as.sequence.flow_style = GTEXT_YAML_FLOW_STYLE_FLOW;
					
					/* Set the count */
					node->as.sequence.count = p->temp.count;
					node_set_source_location(node, source_offset, source_line, source_col);
					
					/* Copy collected items */
					for (size_t i = 0; i < p->temp.count; i++) {
						node->as.sequence.children[i] = p->temp.items[i];
					}
					
					stack_pop(p);
					
					/* Add to parent or set as root */
					if (p->stack.depth == 0) {
						GTEXT_YAML_Status root_status = set_document_root(p, node);
						if (root_status != GTEXT_YAML_OK) return root_status;
					} else {
						GTEXT_YAML_Status supplied =
							block_value_supply_if_missing(
				p, node,
				block_entry_col(p, source_offset, source_col));
						if (supplied != GTEXT_YAML_OK) return supplied;
						if (!temp_add(p, node)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message = "Out of memory nesting sequence";
							}
							return GTEXT_YAML_E_OOM;
						}
						flow_entry_completed(p);
						maybe_finish_block_mapping_value(p);
					}

					p->last_emitted_node = node;
					p->last_emitted_line = event->line;
					break;
				}
				
				case '{':
					/* Start flow mapping (fallback if START event not emitted) */
					if (!stack_push(
						p,
						NULL,
						STATE_MAPPING_KEY,
						NULL,
						NULL,
						-1,
						false,
						event->offset,
						event->line,
						event->col
					)) {
						return stack_push_failed(p, "Out of memory tracking mapping");
					}
					break;
					
				case '}': {
					/* End flow mapping - create node with collected pairs */
					if (p->stack.depth == 0 || 
					    (p->stack.states[p->stack.depth - 1] != STATE_MAPPING_KEY &&
					     p->stack.states[p->stack.depth - 1] != STATE_MAPPING_VALUE)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message = "Unexpected } without matching {";
						}
						return GTEXT_YAML_E_INVALID;
					}
					char *anchor = NULL;
					char *tag = NULL;
					size_t source_offset = 0;
					int source_line = 0;
					int source_col = 0;
					stack_get_and_clear_metadata(
						p,
						&anchor,
						&tag,
						&source_offset,
						&source_line,
						&source_col
					);
					
					/* temp.count should be even (key-value pairs) */
					{
						GTEXT_YAML_Status trailing = mapping_close_trailing_key(p);
						if (trailing != GTEXT_YAML_OK) { gtext_allocator_free(p->ctx->alloc, anchor); gtext_allocator_free(p->ctx->alloc, tag); return trailing; }
					}
					size_t pair_count = p->temp.count / 2;
					GTEXT_YAML_Node *node = yaml_node_new_mapping(
						p->ctx, pair_count, tag, anchor
					);
					gtext_allocator_free(p->ctx->alloc, anchor);
					gtext_allocator_free(p->ctx->alloc, tag);
					if (!node) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_OOM;
							p->error->message = "Out of memory creating mapping";
						}
						return GTEXT_YAML_E_OOM;
					}

					parser_attach_leading_comment(p, node);
					
					node->as.mapping.flow_style = GTEXT_YAML_FLOW_STYLE_FLOW;
					/* Set the count */
					node->as.mapping.count = pair_count;
					node_set_source_location(node, source_offset, source_line, source_col);
					
					/* Copy key-value pairs */
					for (size_t i = 0; i < pair_count; i++) {
						node->as.mapping.pairs[i].key = p->temp.items[i * 2];
						node->as.mapping.pairs[i].value = p->temp.items[i * 2 + 1];
					}
					
					stack_pop(p);
					
					/* Add to parent or set as root */
					if (p->stack.depth == 0) {
						GTEXT_YAML_Status root_status = set_document_root(p, node);
						if (root_status != GTEXT_YAML_OK) return root_status;
					} else {
						if (!temp_add(p, node)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message = "Out of memory nesting mapping";
							}
							return GTEXT_YAML_E_OOM;
						}
						flow_entry_completed(p);
						maybe_finish_block_mapping_value(p);
					}

					p->last_emitted_node = node;
					p->last_emitted_line = event->line;
					break;
				}
				
				case ':':
				{
					/* Mapping key-value separator */
					bool in_flow_mapping = false;
					bool in_block_mapping = false;
					int key_indent = p->last_scalar_key_col >= 0
						? p->last_scalar_key_col
						: p->last_scalar_col;
					GTEXT_YAML_Node *key_node = NULL;
					size_t source_offset = 0;
					int source_line = 0;
					int source_col = 0;
					size_t top = 0;

					if (p->stack.depth > 0) {
						top = p->stack.depth - 1;
						in_flow_mapping = !p->stack.is_block[top] &&
							(p->stack.states[top] == STATE_MAPPING_KEY ||
							 p->stack.states[top] == STATE_MAPPING_VALUE);
						in_block_mapping = p->stack.is_block[top] &&
							(p->stack.states[top] == STATE_MAPPING_KEY ||
							 p->stack.states[top] == STATE_MAPPING_VALUE);
					}

					/* In block context a ":" on the "?"'s own line is not the
					 * explicit entry's colon at all: "?" takes
					 * s-l+block-indented, which includes ns-l-compact-mapping
					 * (8.2.2), so the colon belongs to a mapping nested under
					 * the "?" and that mapping is the explicit key.
					 *
					 *     - ? : x      the key is {null: x}, and the entry
					 *                 that key belongs to has no value
					 *
					 * c-l-block-map-explicit-value needs a line of its own,
					 * so there is nothing else such a colon could be.  Flow
					 * is the other way round: ns-flow-map-explicit-entry
					 * reaches its ":" across s-separate, which needs no
					 * break, and "[ ? a : b ]" is one pair on one line. */
					const bool colon_on_question_line =
						in_block_mapping
						&& (p->explicit_key_active || p->explicit_key_pending)
						&& p->stack.depth == p->explicit_key_depth
						&& p->explicit_key_line == event->line;

					/* A "?" whose key never arrived still has one: the
					 * empty node.  "? " over ": 1" and "[ ? : 1 ]" are
					 * {null: 1}, by c-l-block-map-explicit-entry's
					 * "e-node" arm in block context and by
					 * ns-flow-map-explicit-entry's "( e-node e-node )" in
					 * flow (8.2.2, 7.4).  Both were refused.
					 *
					 * A "?" left behind at a *shallower* level is a
					 * different thing - a key the parser lost track of as
					 * collections opened under it - and stays an error. */
					if (p->explicit_key_pending
							&& !colon_on_question_line
							&& p->stack.depth == p->explicit_key_depth
							&& p->stack.depth > 0) {
						if (!mapping_supply_empty_key(p)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message =
									"Out of memory completing explicit key";
							}
							return GTEXT_YAML_E_OOM;
						}
						p->explicit_key_pending = false;
						p->explicit_key_active = true;
						p->stack.states[p->stack.depth - 1] = STATE_MAPPING_KEY;
					}

					if (p->explicit_key_pending
							&& !colon_on_question_line
							&& p->stack.depth <= p->explicit_key_depth) {
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message = "Explicit key missing before ':'";
						}
						return GTEXT_YAML_E_INVALID;
					}

					/* An explicit key's value colon stands on a line of its own
					 * (c-l-block-map-explicit-value is s-indent(n) ":" ...).
					 * A ":" with a scalar in front of it on the same line
					 * belongs to that scalar, so the explicit key above it
					 * simply never got a value: "? a" over "? b" over "c:"
					 * is three keys, and was being refused because b's colon
					 * was looked for in c's. */
					if (p->explicit_key_active && in_block_mapping
							&& p->stack.depth == p->explicit_key_depth
							&& p->last_scalar_line == event->line
							/* ... but not when the "?" is on this line too.
							 * Then the scalar is the first key of a compact
							 * mapping that is the explicit key, not a new
							 * entry beside it - suite case V9D5. */
							&& !colon_on_question_line) {
						/* The scalar on this line is already in temp, sitting
						 * where the explicit key's value belongs, so it has to
						 * step aside while the null goes in behind it. */
						GTEXT_YAML_Node *implicit_key = NULL;
						if (p->temp.count > 0) {
							implicit_key = p->temp.items[--p->temp.count];
						}
						if (!mapping_supply_null_value(p)
								|| (implicit_key && !temp_add(p, implicit_key))) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message = "Out of memory completing explicit key";
							}
							return GTEXT_YAML_E_OOM;
						}
						p->explicit_key_active = false;
						if (p->stack.depth > 0) {
							p->stack.states[p->stack.depth - 1] = STATE_MAPPING_KEY;
						}
					}

					/* c-l-block-map-explicit-value(n) is s-indent(n) ":" - a
					 * line of its own (8.2.2) - so in block context a ":"
					 * sharing the "?"'s line is not the explicit key's value
					 * colon.  It belongs to the compact mapping the "?"
					 * opened, and is that mapping's first key's colon:
					 * "? earth: blue" has {earth: blue} for its key.
					 *
					 * Flow is the other way round.  ns-flow-map-explicit-entry
					 * reaches its ":" through s-separate, which needs no
					 * break, so "[ ? a : b ]" is one line and is a pair. */
					if (p->explicit_key_active
							&& p->stack.depth == p->explicit_key_depth
							&& !colon_on_question_line) {
						if (p->stack.depth == 0) {
							if (p->error) {
								p->error->code = GTEXT_YAML_E_INVALID;
								p->error->message = "Explicit key missing mapping context";
							}
							return GTEXT_YAML_E_INVALID;
						}
						if (in_block_mapping && p->stack.indents[top] != event->col) {
							if (p->error) {
								p->error->code = GTEXT_YAML_E_INVALID;
								p->error->message = "Explicit key ':' indentation mismatch";
							}
							return GTEXT_YAML_E_INVALID;
						}
						p->stack.states[top] = STATE_MAPPING_VALUE;
						p->expect_mapping_value = true;
						p->explicit_key_active = false;
						break;
					}

					/* A single-pair entry of a flow sequence keeps its key
					 * and its ":" on one line.  "[a: 1]" is ns-flow-pair,
					 * whose key is ns-s-implicit-yaml-key(c) - a node
					 * followed by s-separate-in-line?, which is white space
					 * with no break in it (7.4).  So
					 *
					 *     [ key
					 *       : value ]
					 *
					 * has no production, and it parsed as {"key": "value"}
					 * (suite cases DK4H and ZXT5).
					 *
					 * A flow *mapping* is the opposite case and is left
					 * alone.  ns-flow-map-yaml-key-entry reaches its ":"
					 * across s-separate(n,c), which may hold a line break,
					 * so {"foo"\n: "bar"} is well formed - the first draft
					 * of this rule refused it and seven cases like it.
					 *
					 * An explicit key is exempt too: "? a" over ": b" is
					 * what the "?" is for.  A key that is a collection
					 * rather than a scalar is not covered, because it is the
					 * pending scalar that carries the line.
					 *
					 * Two of the tests below cannot be made to change an
					 * answer today, and are kept for scope rather than
					 * effect.  !is_block keeps the rule where ns-flow-pair
					 * is - a block sequence's "- key" over ": value" is
					 * already refused, by the rule that a block mapping's
					 * key shares its colon's line.  last_scalar_node keeps
					 * it to a colon that has a key to pair with; a ":" with
					 * no key before it is already refused too.  Both stop
					 * this rule quietly taking over those shapes if the
					 * rules that cover them ever move. */
					if (p->stack.depth > 0
						&& !p->stack.is_block[top]
						&& p->stack.states[top] == STATE_SEQUENCE
						&& !p->explicit_key_active
						&& !p->explicit_key_pending
						&& (p->stack.flow_flags[top] & GTEXT_YAML_FLOW_ITEM_DONE)
						&& p->last_scalar_node != NULL
						&& p->last_scalar_line >= 0
						&& event->line != p->last_scalar_line) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message =
								"Implicit key and its ':' on different lines";
						}
						return GTEXT_YAML_E_INVALID;
					}

					if (in_flow_mapping) {
						/* c-ns-flow-map-empty-key-entry is "e-node" then the
						 * value (7.4), so "{ : 1 }" is {null: 1} and
						 * "{a: 1, : 2}" is {a: 1, null: 2}.  Children are
						 * held as alternating key, value pairs, so an even
						 * count in key state means no key is waiting for
						 * this ":" and the empty node is it.  Without this
						 * the value landed in the key slot and picked up a
						 * null of its own: "{ : 1 }" came out as {1: null},
						 * which is neither the right key nor the right
						 * value.
						 *
						 * The key-state test is a precondition rather than
						 * a branch: a ":" never reaches here in value state
						 * with an even count.  An even count above zero is
						 * exactly what marks the entry complete, so the node
						 * after such a colon is refused for having no ","
						 * before it, and the only even count left is zero,
						 * which the "," resets to key state.  Instrumenting
						 * it across the whole suite found no input that
						 * reaches it.  It stays because supplying a key
						 * where a value is expected would be wrong whatever
						 * made it reachable. */
						if (p->stack.states[top] == STATE_MAPPING_KEY
								&& (p->temp.count % 2) == 0
								&& !mapping_supply_empty_key(p)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message =
									"Out of memory completing flow mapping entry";
							}
							return GTEXT_YAML_E_OOM;
						}
						p->stack.states[top] = STATE_MAPPING_VALUE;
						break;
					}

					/* A ":" directly inside a flow sequence makes that entry a
					 * single-pair mapping: "[a: 1]" is "[{a: 1}]".  The pair
					 * closes at the next "," or "]" rather than at a "}", so
					 * the level is marked to say so.  Without this the ":" fell
					 * through to the block-mapping path below and pushed a
					 * block level inside the sequence, which then swallowed the
					 * "]" that should have closed it. */
					if (p->stack.depth > 0 && !p->stack.is_block[top] &&
						p->stack.states[top] == STATE_SEQUENCE) {
						GTEXT_YAML_Node *pair_key = NULL;
						/* The key is the entry this ":" belongs to, and there
						 * is one only if something has landed since the last
						 * separator - which is what FLOW_ITEM_DONE says.
						 *
						 * Without that test the key was taken from wherever
						 * the last node happened to be, reaching back across
						 * the comma: "[a, : 1]" paired "a" with "1" and came
						 * out as [{"a": 1}], one entry where there are two
						 * and a pair nobody wrote.  Nothing was reported;
						 * the document simply changed meaning. */
						if (p->stack.flow_flags[top] & GTEXT_YAML_FLOW_ITEM_DONE) {
							pair_key = detach_last_scalar(p);
							if (!pair_key && p->temp.count > 0) {
								/* The key may be a collection rather than a
								 * scalar, as in "[[1]: 2]", and
								 * detach_last_scalar() only knows about
								 * scalars. */
								pair_key = p->temp.items[--p->temp.count];
							}
						}
						if (!pair_key) {
							/* c-ns-flow-map-empty-key-entry: e-node, then the
							 * value (7.4).  "[: 1]" is [{null: 1}]. */
							pair_key = yaml_node_new_scalar(
								p->ctx, "", 0, NULL, NULL);
						}
						if (!pair_key) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message =
									"Out of memory starting flow pair";
							}
							return GTEXT_YAML_E_OOM;
						}
						size_t pair_offset = 0;
						int pair_line = 0, pair_col = 0;
						node_get_source_location(pair_key, &pair_offset, &pair_line, &pair_col);
						if (!stack_push(p, NULL, STATE_MAPPING_VALUE, NULL, NULL,
								-1, false, pair_offset, pair_line, pair_col)) {
							return stack_push_failed(
								p, "Out of memory starting flow pair");
						}
						p->stack.flow_flags[p->stack.depth - 1] |= GTEXT_YAML_FLOW_PAIR;
						if (!temp_add(p, pair_key)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message = "Out of memory starting flow pair";
							}
							return GTEXT_YAML_E_OOM;
						}
						p->expect_mapping_value = true;
						break;
					}

					/* The other arm of c-l-block-map-implicit-entry is
					 * e-node (8.2.2), so a block mapping entry may have no
					 * key written at all.  ": a" is {null: a}, ":" alone is
					 * {null: null}, and a mapping already open takes such an
					 * entry beside its others:
					 *
					 *     key: value
					 *     : empty key
					 *
					 * A ":" has no key when no scalar stands in front of it
					 * on its own line - either none has been read, or the
					 * last one belongs to a line already finished.  A scalar
					 * on this line is this entry's key, and is claimed below.
					 *
					 * An explicit key is not this shape, except for the one
					 * the "?" opened on this very line: "?" has already put a
					 * key in place, and its own ":" is handled above.
					 *
					 * One shape is left out on purpose.  A property written
					 * on an earlier line is still looking for its node, and
					 * the node it would get is the mapping this ":" opens -
					 * "&a" on its own line over ": 1" anchors the mapping,
					 * the way it does in "&a" over "a: 1" (8.2).  There is
					 * nowhere to put such a property when no key carries it
					 * in, so that entry stays refused rather than being
					 * accepted with the property moved quietly onto the
					 * value.  The same gap keeps suite cases 26DV and 6BFJ
					 * failing. */
					const bool property_waiting =
						event->prop_col >= 0 && event->prop_line > 0
						&& event->prop_line < event->line;

					if ((key_indent < 0 || p->last_scalar_line != event->line)
							&& colon_begins_its_line(p, event->offset)
							&& !flow_key_candidate(p, event->line)
							&& !property_waiting
							&& (colon_on_question_line
								|| (!p->explicit_key_active
									&& !p->explicit_key_pending))) {
						if (!colon_on_question_line && in_block_mapping
								&& p->stack.indents[top] == event->col) {
							/* Another entry of the mapping already open.  In
							 * value state the key above never got one, and
							 * gets the empty node before this entry starts. */
							if (p->stack.states[top] == STATE_MAPPING_VALUE
									&& (p->temp.count % 2) == 1) {
								if (!mapping_supply_null_value(p)) {
									p->failed = true;
									if (p->error) {
										p->error->code = GTEXT_YAML_E_OOM;
										p->error->message =
											"Out of memory closing mapping entry";
									}
									return GTEXT_YAML_E_OOM;
								}
							}
							if (!mapping_supply_empty_key(p)) {
								p->failed = true;
								if (p->error) {
									p->error->code = GTEXT_YAML_E_OOM;
									p->error->message =
										"Out of memory supplying empty key";
								}
								return GTEXT_YAML_E_OOM;
							}
							p->claimed_key = p->temp.items[p->temp.count - 1];
							p->stack.states[top] = STATE_MAPPING_VALUE;
							p->expect_mapping_value = true;
							break;
						}

						/* ...or deeper than a block mapping whose last key is
						 * still waiting for a value, where this ":" opens the
						 * nested mapping that value is:
						 *
						 *     a:
						 *       : 1      # {"a": {null: 1}}
						 *
						 * The e-node arm of ns-l-block-map-implicit-entry
						 * (8.2.2) is reachable at the *first* entry of such a
						 * mapping as much as at any later one, and a later one
						 * already worked - "a:" over "  b: 1" over "  : 2" is
						 * the branch above, which needs the mapping to be open
						 * already.  Nothing opened it for the first entry,
						 * because the branch that opens one ran only where no
						 * block mapping was open at all.
						 *
						 * An odd count is what says a key is outstanding, and
						 * it is what keeps "a: 1" over "  : 2" refused: there
						 * the value has landed, so the deeper ":" belongs to
						 * nothing. */
						const bool opens_nested_value_mapping =
							in_block_mapping
							&& event->col > p->stack.indents[top]
							&& p->stack.states[top] == STATE_MAPPING_VALUE
							&& (p->temp.count % 2) == 1;

						if (colon_on_question_line || !in_block_mapping
								|| opens_nested_value_mapping) {
							/* The ":" opens a block mapping of its own, at
							 * its own column: at the top of a document,
							 * compacted onto a sequence entry as in "- :",
							 * or nested under a "?" on this line, where the
							 * mapping it opens is the explicit key - suite
							 * case M2N8, "- ? : x". */
							if (node_is_on_document_start_line(p, event->offset)) {
								p->failed = true;
								if (p->error) {
									p->error->code = GTEXT_YAML_E_INVALID;
									p->error->message =
										"Block mapping may not begin on the \"---\" line";
								}
								return GTEXT_YAML_E_INVALID;
							}
							if (!stack_push(
								p,
								NULL,
								STATE_MAPPING_KEY,
								NULL,
								NULL,
								event->col,
								true,
								event->offset,
								event->line,
								event->col
							)) {
								return stack_push_failed(p, "Out of memory tracking block mapping");
							}
							if (!mapping_supply_empty_key(p)) {
								p->failed = true;
								if (p->error) {
									p->error->code = GTEXT_YAML_E_OOM;
									p->error->message =
										"Out of memory supplying empty key";
								}
								return GTEXT_YAML_E_OOM;
							}
							p->claimed_key = p->temp.items[p->temp.count - 1];
							p->stack.states[p->stack.depth - 1] = STATE_MAPPING_VALUE;
							p->expect_mapping_value = true;
							break;
						}
					}

					/* Everything below measures the last *scalar*, which says
					 * nothing about a key that is a flow collection: "[a]: b"
					 * has no scalar of its own standing before the ":" and
					 * "{}: b" has none at all, so the last scalar is whatever
					 * came before - on whatever line that was.  Where there
					 * is a flow key, measure the collection instead, at the
					 * position the parser recorded when it built it. */
					GTEXT_YAML_Node *flow_key =
						flow_key_candidate(p, event->line);
					const bool has_flow_key = flow_key != NULL;
					size_t key_start_offset = p->last_scalar_offset;

					if (has_flow_key) {
						node_get_source_location(
							flow_key, &key_start_offset, NULL, NULL);
						/* Where the *entry* begins, which is not where the
						   collection does when a property stands in front of
						   it: "&a {}" is a key starting at the "&".  This is
						   the same function the scalar case uses, and taking
						   the collection's own column instead indented the
						   mapping to the "{" - so in "&a {}: 1" over "b: 2"
						   the second entry fell outside the mapping the first
						   had opened and was refused as a second top-level
						   node.  Without the property the two columns agree,
						   which is why it took a fuzzer to find. */
						key_indent = line_key_col_from_offset(p, key_start_offset);
					}

					if (key_indent < 0 && !has_flow_key) {
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message = "Mapping key missing before ':'";
						}
						return GTEXT_YAML_E_INVALID;
					}

					if (p->last_scalar_line != event->line && !has_flow_key) {
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message = "Mapping key not on same line as ':'";
						}
						return GTEXT_YAML_E_INVALID;
					}

					/* The key has to stand where a block entry may start,
					 * for the same reason a "-" does: in
					 * "x: { y: z }in: valid" the second key sits beside a
					 * flow mapping that is already a complete node.
					 *
					 * "a: 1" over "{}: 2" was refused here, for a key
					 * standing beside a node it is nowhere near: the scalar
					 * measured was the "1" on the line above.  "{}: 1" alone
					 * parsed, because with nothing in front of it there was
					 * no earlier scalar to be measured instead. */
					if (in_block_mapping
							&& !block_key_may_start_at(p, key_start_offset)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message =
								"Mapping key beside a node already on this line";
						}
						return GTEXT_YAML_E_INVALID;
					}

					if (in_block_mapping && p->stack.indents[top] == key_indent) {
						/* The scalar before a ":" is its key, and it is held
						 * provisionally as the previous key's value until this
						 * ":" arrives to claim it.  An even count means no such
						 * scalar is outstanding, so this ":" has no key at all:
						 * "key: a : b" used to yield {"key": "a", "b": null},
						 * silently turning the tail of a value into a pair. */
						if ((p->temp.count % 2) == 0) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_INVALID;
								p->error->message = "Mapping key missing before ':'";
							}
							return GTEXT_YAML_E_INVALID;
						}
						p->claimed_key = p->temp.items[p->temp.count - 1];
						p->stack.states[top] = STATE_MAPPING_VALUE;
						p->expect_mapping_value = true;
						break;
					}

					key_node = detach_last_scalar(p);
					if (!key_node) {
						/* The key is the collection, and key_indent already
						   holds where it begins rather than where the last
						   scalar inside it happened to be. */
						key_node = detach_last_flow_node(p, event->line);
					}
					if (!key_node) {
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message = "Mapping key not found before ':'";
						}
						return GTEXT_YAML_E_INVALID;
					}
					node_get_source_location(key_node, &source_offset, &source_line, &source_col);

					/* A key indented further than its mapping starts a nested
					 * mapping only when there is a key above still waiting for
					 * a value to put it under.  A mapping's children are held
					 * as alternating key, value pairs, so with the key just
					 * detached an even count means every key already has one
					 * and this key belongs to nothing.  That used to nest
					 * regardless, which put a mapping where a key should be:
					 * "a: 1" followed by an indented "b: 2" parsed as
					 * {"a": 1, {"b": 2}: null} rather than being refused. */
					if (node_is_on_document_start_line(p, source_offset)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message =
								"Block mapping may not begin on the \"---\" line";
						}
						return GTEXT_YAML_E_INVALID;
					}

					/* ... except under a "?" on this same line, where a key
					 * indented past the mapping is the compact mapping the
					 * "?" opened (ns-l-compact-mapping, 8.2.2) rather than a
					 * key belonging to nothing. */
					if (in_block_mapping && key_indent > p->stack.indents[top] &&
						(p->temp.count % 2) == 0 && !colon_on_question_line) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message =
								"Mapping key indented deeper than its mapping";
						}
						return GTEXT_YAML_E_INVALID;
					}

					if (!stack_push(
						p,
						NULL,
						STATE_MAPPING_KEY,
						NULL,
						NULL,
						key_indent,
						true,
						source_offset,
						source_line,
						source_col
					)) {
						return stack_push_failed(p, "Out of memory tracking block mapping");
					}

					/* An own-line tag before a block mapping reaches the parser
					 * on the mapping's first key, because the key is the first
					 * node the scanner has to attach it to.  It belongs to the
					 * mapping.  Done after the push, so the mapping's stack
					 * entry is the one on top. */
					GTEXT_YAML_Status adopted =
						adopt_own_line_tag(p, key_node);
					if (adopted == GTEXT_YAML_OK) {
						adopted = adopt_own_line_anchor(p, key_node);
					}
					if (adopted != GTEXT_YAML_OK) return adopted;

					if (!temp_add(p, key_node)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_OOM;
							p->error->message = "Out of memory adding key to mapping";
						}
						return GTEXT_YAML_E_OOM;
					}
					p->claimed_key = key_node;

					p->stack.states[p->stack.depth - 1] = STATE_MAPPING_VALUE;
					p->expect_mapping_value = true;
					break;
				}

				case '?':
				{
					bool in_mapping = false;
					bool in_flow_mapping = false;
					bool in_flow_sequence = false;
					bool at_block_mapping = false;
					int indent = event->col;
					size_t top = 0;

					if (p->explicit_key_pending) {
						/* A '?' with no key between it and the last one. */
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message = "Explicit key already pending";
						}
						return GTEXT_YAML_E_INVALID;
					}

					/* A second '?' means the key before it never got a ':', so
					 * its value is null.  This used to be refused outright,
					 * which made "? a" followed by "? b" - and so the block
					 * spelling of !!set - unparseable. */
					if (p->explicit_key_active) {
						if (!mapping_supply_null_value(p)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message = "Out of memory completing explicit key";
							}
							return GTEXT_YAML_E_OOM;
						}
						p->explicit_key_active = false;
						if (p->stack.depth > 0) {
							p->stack.states[p->stack.depth - 1] = STATE_MAPPING_KEY;
						}
					}

					if (p->stack.depth > 0) {
						top = p->stack.depth - 1;
						in_mapping = (p->stack.states[top] == STATE_MAPPING_KEY ||
							p->stack.states[top] == STATE_MAPPING_VALUE);
						in_flow_mapping = in_mapping && !p->stack.is_block[top];
						at_block_mapping = in_mapping && p->stack.is_block[top] &&
							p->stack.indents[top] == indent;
						in_flow_sequence = !p->stack.is_block[top] &&
							p->stack.states[top] == STATE_SEQUENCE;
					}

					if (in_flow_mapping) {
						p->explicit_key_pending = true;
						p->explicit_key_active = false;
						p->explicit_key_indent = indent;
						p->explicit_key_line = event->line;
						p->explicit_key_depth = p->stack.depth;
						break;
					}

					/* A "?" directly inside a flow sequence opens a single-pair
					 * mapping, exactly as a ":" does there.  ns-flow-pair is
					 *
					 *     ( "?" s-separate ns-flow-map-explicit-entry )
					 *   | ns-flow-pair-entry
					 *
					 * (7.4), so "[ ? a : b ]" is "[{a: b}]" and "[ ? a ]" is
					 * "[{a: null}]".  The pair ends where the sequence entry
					 * ends - at the "," or the "]" - rather than at a "}",
					 * which is what GTEXT_YAML_FLOW_PAIR marks; close_flow_pair
					 * is already called from both.
					 *
					 * Without this the "?" fell through to the block arm below
					 * and pushed a *block* mapping inside the flow sequence.
					 * That level then measured itself by indentation, which
					 * says nothing inside "[" and "]", and swallowed the
					 * bracket that should have closed the sequence. */
					if (in_flow_sequence) {
						if (!stack_push(
							p,
							NULL,
							STATE_MAPPING_KEY,
							NULL,
							NULL,
							-1,
							false,
							event->offset,
							event->line,
							event->col
						)) {
							return stack_push_failed(p, "Out of memory starting flow pair");
						}
						p->stack.flow_flags[p->stack.depth - 1] |=
							GTEXT_YAML_FLOW_PAIR;
						p->explicit_key_pending = true;
						p->explicit_key_active = false;
						p->explicit_key_indent = indent;
						p->explicit_key_line = event->line;
						p->explicit_key_depth = p->stack.depth;
						break;
					}

					if (!at_block_mapping) {
						if (!stack_push(
							p,
							NULL,
							STATE_MAPPING_KEY,
							NULL,
							NULL,
							indent,
							true,
							event->offset,
							event->line,
							event->col
						)) {
							return stack_push_failed(p, "Out of memory tracking explicit key");
						}
					}

					p->explicit_key_pending = true;
					p->explicit_key_active = false;
					p->explicit_key_indent = indent;
					p->explicit_key_line = event->line;
					p->explicit_key_depth = p->stack.depth;
					break;
				}
					
				case ',':
					/* A "," belongs to a flow collection and nowhere else:
					 * c-flow-sequence and c-flow-mapping are the only
					 * productions that hold one, and ns-plain-first excludes
					 * c-indicator, so a plain scalar cannot begin with one
					 * either (7.4, 7.3.3).  Outside "[" or "{" it was simply
					 * ignored, which is how "- !!str, xxx" came back as the
					 * one-entry sequence ["xxx"]: the tag name stops at the
					 * "," correctly, and then the "," itself vanished
					 * (suite case U99R).
					 *
					 * A "," inside a plain scalar is a different thing and
					 * does not reach here - ns-plain-char allows it in block
					 * context, so "a,b" is one scalar. */
					if (p->stack.depth == 0
						|| p->stack.is_block[p->stack.depth - 1]) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message =
								"',' outside a flow collection";
						}
						return GTEXT_YAML_E_INVALID;
					}

					/* A separator separates two entries, so there has to be
					 * one in front of it. An empty entry was being dropped
					 * silently, so "[ , a, b, c ]" and "[ a, b, c, , ]" both
					 * parsed as the three-entry sequence. A flow mapping's
					 * key with no value yet counts as an entry - "{a, b}" is
					 * two keys - which an odd child count is what says. */
					if (p->stack.depth > 0 && !p->stack.is_block[p->stack.depth - 1]) {
						const size_t sep_top = p->stack.depth - 1;
						const bool in_flow_map =
							p->stack.states[sep_top] == STATE_MAPPING_KEY ||
							p->stack.states[sep_top] == STATE_MAPPING_VALUE;
						/* A "?" starts an entry even before its key
						 * arrives, and an entry whose key never arrives is
						 * still one: "[ ? , ? ]" is two pairs of empty
						 * nodes. "[ , ]" holds nothing at all and stays an
						 * error. */
						const bool started_explicit =
							(p->explicit_key_pending || p->explicit_key_active)
							&& p->explicit_key_depth == p->stack.depth;
						const bool have_entry =
							(p->stack.flow_flags[sep_top] & GTEXT_YAML_FLOW_ITEM_DONE)
							|| (in_flow_map && (p->temp.count % 2) == 1)
							|| started_explicit;
						if (!have_entry) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_INVALID;
								p->error->message =
									"Flow collection entry missing before ','";
							}
							return GTEXT_YAML_E_INVALID;
						}
					}
					/* Item separator - handle mapping state flip */
					{
						GTEXT_YAML_Status pair_status = close_flow_pair(p);
						if (pair_status != GTEXT_YAML_OK) return pair_status;
					}
					if (p->stack.depth > 0) {
						p->stack.flow_flags[p->stack.depth - 1] &=
							(unsigned char)~GTEXT_YAML_FLOW_ITEM_DONE;
						const size_t sep_top = p->stack.depth - 1;
						const bool in_flow_map = !p->stack.is_block[sep_top] &&
							(p->stack.states[sep_top] == STATE_MAPPING_KEY ||
							 p->stack.states[sep_top] == STATE_MAPPING_VALUE);
						/* An entry that reached the comma without a ':' is a key
						 * whose value is null, the same rule a block mapping
						 * follows.  Children are held as alternating key, value
						 * pairs, so an odd count means the entry just closed has
						 * no value yet.  Without this, "{a, b}" paired the two
						 * keys with each other. */
						/* A "?" whose key never arrived still leaves an
						 * entry behind at the comma, both halves empty:
						 * "{ ? , a }" is {null: null, a: null}.  The key has
						 * to go in before the rule below, which is what
						 * turns the odd count into the null value. */
						if (in_flow_map
								&& p->explicit_key_pending
								&& p->explicit_key_depth == p->stack.depth
								&& (p->temp.count % 2) == 0) {
							if (!mapping_supply_empty_key(p)) {
								p->failed = true;
								if (p->error) {
									p->error->code = GTEXT_YAML_E_OOM;
									p->error->message =
										"Out of memory completing explicit key";
								}
								return GTEXT_YAML_E_OOM;
							}
							p->explicit_key_pending = false;
						}
						if (in_flow_map && (p->temp.count % 2) == 1) {
							if (!mapping_supply_null_value(p)) {
								p->failed = true;
								if (p->error) {
									p->error->code = GTEXT_YAML_E_OOM;
									p->error->message =
										"Out of memory completing flow mapping entry";
								}
								return GTEXT_YAML_E_OOM;
							}
							explicit_key_entry_done(p);
						}
						if (p->stack.states[sep_top] == STATE_MAPPING_VALUE) {
							p->stack.states[sep_top] = STATE_MAPPING_KEY;
						}
					}
					break;
					
				case '-':
				{
					/* Block sequence indicator */
					bool start_new = true;
					int indent = event->col;

					if (!block_entry_may_start_at(p, event->offset)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message =
								"Block sequence entry beside a node already on this line";
						}
						return GTEXT_YAML_E_INVALID;
					}

					/* Inside "[" or "{" there are only flow nodes:
					 * c-flow-sequence holds ns-flow-seq-entry, and a block
					 * sequence is not one of them (7.4).  A "-" there is
					 * either plain content or an error, never an entry
					 * indicator, and "[" over "- a" over "]" was building a
					 * block sequence inside the flow one and giving
					 * [["a"]].  Both references refuse it. */
					if (p->stack.depth > 0
						&& !p->stack.is_block[p->stack.depth - 1]) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message =
								"Block sequence entry inside a flow collection";
						}
						return GTEXT_YAML_E_INVALID;
					}

					if (p->stack.depth > 0) {
						size_t top = p->stack.depth - 1;
						if (p->stack.is_block[top] &&
							p->stack.states[top] == STATE_SEQUENCE &&
							p->stack.indents[top] == indent) {
							start_new = false;
						}
					}

					/* A block mapping's entries are pairs, so a "-" at the
					 * mapping's own column with every key already paired has
					 * nothing to be an entry of. It was becoming a sequence
					 * standing where a key belongs: "a: 1" over "- b" gave
					 * {"a": 1, ["b"]: null}, a sequence as a mapping key,
					 * which neither PyYAML nor js-yaml will parse. A key
					 * still waiting for its value is the ordinary case -
					 * "a:" over "- b" is that sequence as a's value - and an
					 * odd temp count is what tells them apart. */
					/* ... unless a "?" at this level is still waiting for its
					 * key, in which case the sequence is that key.  It may
					 * stand at the mapping's own column: "?" takes
					 * s-l+block-indented(n,block-out), whose s-l+block-node
					 * arm reaches a block sequence through
					 * seq-spaces(n,block-out), which is n-1 (8.2.1, 8.2.2).
					 * Suite case 6PBE writes one at column zero. */
					const bool key_of_pending_explicit_entry =
						p->explicit_key_pending
						&& p->stack.depth == p->explicit_key_depth;

					if (start_new && p->stack.depth > 0
							&& !key_of_pending_explicit_entry) {
						const size_t top = p->stack.depth - 1;
						if (p->stack.is_block[top] &&
							(p->stack.states[top] == STATE_MAPPING_KEY ||
							 p->stack.states[top] == STATE_MAPPING_VALUE) &&
							p->stack.indents[top] >= 0 &&
							indent <= p->stack.indents[top] &&
							(p->temp.count % 2) == 0) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_INVALID;
								p->error->message =
									"Block sequence entry where a mapping key belongs";
							}
							return GTEXT_YAML_E_INVALID;
						}
					}

					/* A nested sequence needs something waiting to hold it.
					 * Inside a block sequence that is the entry indicator
					 * above it, still open; once that entry has its node, a
					 * "-" indented past the sequence belongs to nothing:
					 *
					 *     - key: value
					 *      - item1
					 *
					 * became a second entry holding ["item1"] (suite case
					 * ZVH3). l+block-sequence(n) is a run of s-indent(n)
					 * c-l-block-seq-entry(n), so every entry of one sequence
					 * is at the same column, and a deeper one has to be some
					 * other node's value.
					 *
					 * A "-" after a *scalar* entry never reaches here: it
					 * folds into that scalar as plain content, which is why
					 * "- a" over " - b" is the one string "a - b".
					 *
					 * "Deeper" needs no test of its own. start_new is false
					 * when the "-" is at the sequence's own column, and a
					 * "-" left of it closed the sequence before reaching
					 * this, so start_new already means deeper.  Neither does
					 * "block": the rule above has already refused a "-"
					 * inside a flow collection. */
					if (start_new && p->stack.depth > 0) {
						const size_t top = p->stack.depth - 1;
						if (p->stack.states[top] == STATE_SEQUENCE &&
							!(p->stack.flow_flags[top]
								& GTEXT_YAML_BLOCK_ENTRY_OPEN)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_INVALID;
								p->error->message =
									"Block sequence entry indented past its sequence";
							}
							return GTEXT_YAML_E_INVALID;
						}
					}

					/* A "-" at this level with nothing under the one before
					 * it means that entry was empty. */
					if (!start_new
							&& (p->stack.flow_flags[p->stack.depth - 1]
								& GTEXT_YAML_BLOCK_ENTRY_OPEN)) {
						if (!sequence_supply_empty_entry(p)) {
							p->failed = true;
							if (p->error) {
								p->error->code = GTEXT_YAML_E_OOM;
								p->error->message = "Out of memory adding empty entry";
							}
							return GTEXT_YAML_E_OOM;
						}
					}

					if (start_new
						&& node_is_on_document_start_line(p, event->offset)) {
						p->failed = true;
						if (p->error) {
							p->error->code = GTEXT_YAML_E_INVALID;
							p->error->message =
								"Block sequence may not begin on the \"---\" line";
						}
						return GTEXT_YAML_E_INVALID;
					}

					if (start_new) {
						if (!stack_push(
							p,
							NULL,
							STATE_SEQUENCE,
							NULL,
							NULL,
							indent,
							true,
							event->offset,
							event->line,
							event->col
						)) {
							return stack_push_failed(p, "Out of memory tracking block sequence");
						}
					}
					p->stack.flow_flags[p->stack.depth - 1] |=
						GTEXT_YAML_BLOCK_ENTRY_OPEN;
					break;
				}
					
				default:
					/* Unknown indicator - ignore */
					break;
			}
			break;
		}
	}
	
	return GTEXT_YAML_OK;
}

/**
 * @brief Parse YAML string into DOM document (internal implementation).
 */
GTEXT_YAML_Document *yaml_parse_document(
	const char *input,
	size_t length,
	const GTEXT_YAML_Parse_Options *options,
	GTEXT_YAML_Error *error
) {
	if (!input) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Input string is NULL";
		}
		return NULL;
	}
	
	GTEXT_YAML_Parse_Options effective_opts =
		gtext_yaml_parse_options_effective(options);
	const GTEXT_YAML_Parse_Options *opts = &effective_opts;

	/* The fast path builds the document out of a JSON DOM, which collapses
	   duplicate keys however it is asked not to, so it cannot answer for
	   KEEP_ALL.  Taking the ordinary path is slower and right. */
	if (opts->enable_json_fast_path
			&& opts->dupkeys != GTEXT_YAML_DUPKEY_KEEP_ALL
			&& json_fastpath_candidate(input, length)) {
		GTEXT_YAML_Document *json_doc = yaml_parse_json_document_internal(
			input,
			length,
			opts,
			error,
			false
		);
		if (json_doc) {
			return json_doc;
		}
	}
	
	/* Create context */
	yaml_context *ctx = yaml_context_new(opts->allocator);
	if (!ctx) {
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory creating context";
		}
		return NULL;
	}
	
	/* Create document */
	GTEXT_YAML_Document *doc = (GTEXT_YAML_Document *)yaml_context_alloc(
		ctx, sizeof(GTEXT_YAML_Document), 8
	);
	if (!doc) {
		yaml_context_free(ctx);
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory creating document";
		}
		return NULL;
	}
	
	memset(doc, 0, sizeof(*doc));
	doc->ctx = ctx;
	doc->options = *opts;
	doc->document_index = 0;  /* Always parsing first document */
	
	/* Initialize parser state */
	parser_state parser;
	if (!parser_init(&parser, ctx, error)) {
		yaml_context_free(ctx);
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory initializing parser";
		}
		return NULL;
	}
	parser.doc = doc;
	
	/* Create streaming parser */
	GTEXT_YAML_Stream *stream = gtext_yaml_stream_new(opts, parse_callback, &parser);
	if (!stream) {
		parser_free(&parser);
		yaml_context_free(ctx);
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory creating stream parser";
		}
		return NULL;
	}
	/* The parser reads the decoded stream by offset, so it has to still be
	   there. Asked for before the first feed, while nothing has been
	   dropped. */
	gtext_yaml_stream_retain_decoded_input(stream);
	
	/* Enable synchronous mode so aliases can be processed immediately */
	gtext_yaml_stream_set_sync_mode(stream, true);
	
	/* Feed input to streaming parser */
	GTEXT_YAML_Status status = gtext_yaml_stream_feed(stream, input, length);
	if (status == GTEXT_YAML_OK) {
		status = gtext_yaml_stream_finish(stream);
	}
	
	GTEXT_YAML_Error scan_err = stream_take_error(stream);
	gtext_yaml_stream_free(stream);
	
	/* Finalize any open block collections */
	if (status == GTEXT_YAML_OK && !parser.failed) {
		status = close_block_contexts(&parser, -1);
	}
	if (status == GTEXT_YAML_OK && !parser.failed) {
		status = check_outer_property_claimed(&parser);
	}
	if (status == GTEXT_YAML_OK) {
		status = check_flow_contexts_closed(&parser);
	}
	
	/* Check if parsing succeeded */
	if (status != GTEXT_YAML_OK || parser.failed) {
		parser_free(&parser);
		yaml_context_free(ctx);
		parse_error_fallback(error, status, &scan_err);
		return NULL;
	}
	
	/* Resolve all alias nodes */
	status = resolve_aliases(&parser);
	if (status != GTEXT_YAML_OK) {
		parser_free(&parser);
		yaml_context_free(ctx);
		/* Error already set by resolve_aliases */
		return NULL;
	}
	
	/* Set document root */
	doc->root = parser.root;

	if (!finalize_tag_handles(&parser, doc)) {
		parser_free(&parser);
		yaml_context_free(ctx);
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory finalizing tag handles";
		}
		return NULL;
	}

	/* Resolve tags and implicit scalar types */
	status = yaml_resolve_document(doc, error);
	if (status != GTEXT_YAML_OK) {
		parser_free(&parser);
		yaml_context_free(ctx);
		return NULL;
	}

	/* After yaml_resolve_document(), not before: a merge key expands into a
	   mapping node that resolution creates (yaml_resolve.c), so a count taken
	   above misses it.  The context holds one count per document - every path
	   that builds a document gives it a context of its own - so this is that
	   document's own total and not a running one. */
	doc->node_count = ctx->node_count;

	parser_free(&parser);
	return doc;
}

/* ==========================================================================
 * Partial Parser (gtext_yaml_parse_partial)
 * ==========================================================================
 */

typedef struct {
	yaml_context *ctx;
	GTEXT_YAML_Document *doc;
	parser_state parser;
	bool parser_ready;
	bool recovering;
	bool in_document;
	GTEXT_YAML_Error last_error;
	GTEXT_YAML_Error *errors;
	size_t error_count;
	size_t error_capacity;
	GTEXT_YAML_Node **top_nodes;
	size_t top_count;
	size_t top_capacity;
} partial_state;

static bool partial_errors_reserve(partial_state *state, size_t needed) {
	if (!state) return false;
	if (state->error_capacity >= needed) return true;

	/* Doubling, then clamped up to whatever was actually asked for.
	 * The loop this replaces - double until it fits, giving up above
	 * SIZE_MAX/2 - could not iterate: both callers ask for count + 1
	 * and count <= capacity always, so one doubling is always enough
	 * and the body was four of the lines tools/coverage.sh reports as
	 * never executed. Clamping is correct for any `needed`, including
	 * a jump a future caller might ask for, so this is the more
	 * general of the two as well as the reachable one. */
	size_t new_capacity = state->error_capacity == 0 ? 4 : state->error_capacity * 2;
	if (new_capacity < needed) new_capacity = needed;

	GTEXT_YAML_Error *errors = (GTEXT_YAML_Error *)gtext_allocator_realloc(state->ctx->alloc, 
		state->errors, new_capacity * sizeof(*errors)
	);
	if (!errors) return false;
	state->errors = errors;
	state->error_capacity = new_capacity;
	return true;
}

static bool partial_errors_push(partial_state *state, const GTEXT_YAML_Error *err) {
	if (!state || !err) return false;
	if (!partial_errors_reserve(state, state->error_count + 1)) return false;

	GTEXT_YAML_Error *dst = &state->errors[state->error_count];
	memset(dst, 0, sizeof(*dst));
	*dst = *err;

	if (err->context_snippet && err->context_snippet_len > 0) {
		size_t len = err->context_snippet_len;
		char *copy = (char *)gtext_allocator_malloc(state->ctx->alloc, len + 1);
		if (!copy) return false;
		memcpy(copy, err->context_snippet, len);
		copy[len] = '\0';
		dst->context_snippet = copy;
		dst->context_snippet_len = len;
	} else {
		dst->context_snippet = NULL;
		dst->context_snippet_len = 0;
		dst->caret_offset = 0;
	}

	state->error_count++;
	return true;
}

static bool partial_top_reserve(partial_state *state, size_t needed) {
	if (!state) return false;
	if (state->top_capacity >= needed) return true;

	/* Doubling, then clamped up to whatever was actually asked for.
	 * The loop this replaces - double until it fits, giving up above
	 * SIZE_MAX/2 - could not iterate: both callers ask for count + 1
	 * and count <= capacity always, so one doubling is always enough
	 * and the body was four of the lines tools/coverage.sh reports as
	 * never executed. Clamping is correct for any `needed`, including
	 * a jump a future caller might ask for, so this is the more
	 * general of the two as well as the reachable one. */
	size_t new_capacity = state->top_capacity == 0 ? 4 : state->top_capacity * 2;
	if (new_capacity < needed) new_capacity = needed;

	GTEXT_YAML_Node **nodes = (GTEXT_YAML_Node **)gtext_allocator_realloc(state->ctx->alloc, 
		state->top_nodes, new_capacity * sizeof(*nodes)
	);
	if (!nodes) return false;
	state->top_nodes = nodes;
	state->top_capacity = new_capacity;
	return true;
}

static bool partial_top_push(partial_state *state, GTEXT_YAML_Node *node) {
	if (!state || !node) return false;
	if (!partial_top_reserve(state, state->top_count + 1)) return false;
	state->top_nodes[state->top_count++] = node;
	return true;
}

static void partial_capture_root(partial_state *state) {
	parser_state *p = NULL;
	if (!state || !state->parser_ready) return;
	p = &state->parser;
	if (!p->root) return;
	if (!partial_top_push(state, p->root)) return;
	p->root = NULL;
	p->last_scalar_node = NULL;
	p->last_scalar_in_root = false;
	p->last_scalar_in_temp = false;
	p->last_scalar_temp_depth = 0;
	if (p->pending_leading_comment) {
		gtext_allocator_free(state->ctx->alloc, p->pending_leading_comment);
		p->pending_leading_comment = NULL;
	}
}

static void partial_error_fallback(
	GTEXT_YAML_Error *err,
	const GTEXT_YAML_Event *event,
	GTEXT_YAML_Status status
) {
	if (!err) return;
	err->code = status != GTEXT_YAML_OK ? status : GTEXT_YAML_E_INVALID;
	err->message = "Parse error";
	err->offset = event ? event->offset : 0;
	err->line = event ? event->line : 0;
	err->col = event ? event->col : 0;
	err->context_snippet = NULL;
	err->context_snippet_len = 0;
	err->caret_offset = 0;
	err->expected_token = NULL;
	err->actual_token = NULL;
}

static void partial_add_error_marker(partial_state *state, const GTEXT_YAML_Error *err) {
	if (!state || !state->ctx || !err) return;
	const char *message = err->message ? err->message : "parse error";
	char buf[256];
	snprintf(buf, sizeof(buf), "error: %s", message);
	GTEXT_YAML_Node *node = yaml_node_new_scalar(state->ctx, buf, strlen(buf), NULL, NULL);
	if (!node) return;
	node_set_source_location(node, err->offset, err->line, err->col);

	char comment[128];
	snprintf(comment, sizeof(comment), "parse_error line %d col %d", err->line, err->col);
	const char *stored = parser_arena_strdup(state->ctx, comment, strlen(comment));
	if (stored) {
		node->as.scalar.leading_comment = stored;
	}
	partial_top_push(state, node);
}

static bool partial_event_starts_node(const GTEXT_YAML_Event *event) {
	if (!event) return false;
	switch (event->type) {
		case GTEXT_YAML_EVENT_SCALAR:
		case GTEXT_YAML_EVENT_SEQUENCE_START:
		case GTEXT_YAML_EVENT_MAPPING_START:
		case GTEXT_YAML_EVENT_ALIAS:
			return true;
		case GTEXT_YAML_EVENT_INDICATOR:
			switch (event->data.indicator) {
				case '-':
				case '?':
				case ':':
				case '[':
				case '{':
					return true;
				default:
					return false;
			}
		default:
			return false;
	}
}

static bool partial_reset_parser(partial_state *state) {
	if (!state || !state->parser_ready) return false;
	parser_free(&state->parser);
	if (!parser_init(&state->parser, state->ctx, &state->last_error)) return false;
	state->parser.doc = state->doc;
	state->parser.document_started = state->in_document;
	state->parser.first_document_complete = false;
	return true;
}

static GTEXT_YAML_Status partial_callback(
	GTEXT_YAML_Stream *s,
	const void *event_payload,
	void *user_data
) {
	(void)s;
	partial_state *state = (partial_state *)user_data;
	const GTEXT_YAML_Event *event = (const GTEXT_YAML_Event *)event_payload;

	if (!state || !event) return GTEXT_YAML_E_INVALID;

	switch (event->type) {
		case GTEXT_YAML_EVENT_STREAM_START:
			return GTEXT_YAML_OK;
		case GTEXT_YAML_EVENT_DOCUMENT_START:
			state->in_document = true;
			if (state->recovering) {
				partial_reset_parser(state);
				state->recovering = false;
			}
			return GTEXT_YAML_OK;
		case GTEXT_YAML_EVENT_DOCUMENT_END:
			if (state->parser_ready) {
				GTEXT_YAML_Status close_status = close_block_contexts(&state->parser, -1);
				if (close_status != GTEXT_YAML_OK) {
					if (close_status == GTEXT_YAML_E_OOM || close_status == GTEXT_YAML_E_LIMIT) {
						return close_status;
					}
					GTEXT_YAML_Error captured = state->last_error;
					if (captured.code == GTEXT_YAML_OK) {
						partial_error_fallback(&captured, event, close_status);
					}
					if (!partial_errors_push(state, &captured)) {
						return GTEXT_YAML_E_OOM;
					}
					partial_add_error_marker(state, &captured);
				}
			}
			partial_capture_root(state);
			state->in_document = false;
			return GTEXT_YAML_OK;
		case GTEXT_YAML_EVENT_STREAM_END:
			if (state->parser_ready) {
				GTEXT_YAML_Status close_status = close_block_contexts(&state->parser, -1);
				if (close_status != GTEXT_YAML_OK) {
					if (close_status == GTEXT_YAML_E_OOM || close_status == GTEXT_YAML_E_LIMIT) {
						return close_status;
					}
					GTEXT_YAML_Error captured = state->last_error;
					if (captured.code == GTEXT_YAML_OK) {
						partial_error_fallback(&captured, event, close_status);
					}
					if (!partial_errors_push(state, &captured)) {
						return GTEXT_YAML_E_OOM;
					}
					partial_add_error_marker(state, &captured);
				}
			}
			partial_capture_root(state);
			return GTEXT_YAML_OK;
		default:
			break;
	}

	if (!state->in_document) {
		state->in_document = true;
		state->parser.document_started = true;
	}

	if (state->recovering) {
		if (event->col == 1 && partial_event_starts_node(event)) {
			partial_reset_parser(state);
			state->recovering = false;
		} else {
			return GTEXT_YAML_OK;
		}
	}

	GTEXT_YAML_Status status = parse_callback(s, event_payload, &state->parser);
	if (status != GTEXT_YAML_OK) {
		if (status == GTEXT_YAML_E_OOM || status == GTEXT_YAML_E_LIMIT) {
			return status;
		}
		GTEXT_YAML_Error captured = state->last_error;
		if (captured.code == GTEXT_YAML_OK) {
			partial_error_fallback(&captured, event, status);
		}
		if (!partial_errors_push(state, &captured)) {
			return GTEXT_YAML_E_OOM;
		}
		partial_capture_root(state);
		partial_add_error_marker(state, &captured);
		state->recovering = true;
		return GTEXT_YAML_OK;
	}

	return GTEXT_YAML_OK;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_parse_partial(
	const void *data,
	size_t len,
	const GTEXT_YAML_Parse_Options *options,
	GTEXT_YAML_Document **out_doc,
	GTEXT_YAML_Error **out_errors,
	size_t *out_error_count,
	GTEXT_YAML_Error *out_err
) {
	const char *input = (const char *)data;
	if (!input || !out_doc || !out_errors || !out_error_count) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "Invalid arguments";
		}
		return GTEXT_YAML_E_INVALID;
	}

	*out_doc = NULL;
	*out_errors = NULL;
	*out_error_count = 0;

	GTEXT_YAML_Parse_Options effective_opts =
		gtext_yaml_parse_options_effective(options);
	const GTEXT_YAML_Parse_Options *opts = &effective_opts;

	partial_state state;
	memset(&state, 0, sizeof(state));

	state.ctx = yaml_context_new(opts->allocator);
	if (!state.ctx) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_OOM;
			out_err->message = "Out of memory creating context";
		}
		return GTEXT_YAML_E_OOM;
	}

	state.doc = (GTEXT_YAML_Document *)yaml_context_alloc(
		state.ctx, sizeof(GTEXT_YAML_Document), 8
	);
	if (!state.doc) {
		yaml_context_free(state.ctx);
		if (out_err) {
			out_err->code = GTEXT_YAML_E_OOM;
			out_err->message = "Out of memory creating document";
		}
		return GTEXT_YAML_E_OOM;
	}
	memset(state.doc, 0, sizeof(*state.doc));
	state.doc->ctx = state.ctx;
	state.doc->options = *opts;
	state.doc->document_index = 0;

	if (!parser_init(&state.parser, state.ctx, &state.last_error)) {
		yaml_context_free(state.ctx);
		if (out_err) {
			out_err->code = GTEXT_YAML_E_OOM;
			out_err->message = "Out of memory initializing parser";
		}
		return GTEXT_YAML_E_OOM;
	}
	state.parser_ready = true;
	state.parser.doc = state.doc;

	GTEXT_YAML_Stream *stream = gtext_yaml_stream_new(opts, partial_callback, &state);
	if (!stream) {
		parser_free(&state.parser);
		yaml_context_free(state.ctx);
		if (out_err) {
			out_err->code = GTEXT_YAML_E_OOM;
			out_err->message = "Out of memory creating stream parser";
		}
		return GTEXT_YAML_E_OOM;
	}
	gtext_yaml_stream_retain_decoded_input(stream);
	gtext_yaml_stream_set_sync_mode(stream, true);

	GTEXT_YAML_Status status = gtext_yaml_stream_feed(stream, input, len);
	if (status == GTEXT_YAML_OK) {
		status = gtext_yaml_stream_finish(stream);
	}
	GTEXT_YAML_Error scan_err = stream_take_error(stream);
	gtext_yaml_stream_free(stream);

	partial_capture_root(&state);

	if (status != GTEXT_YAML_OK) {
		parser_free(&state.parser);
		if (out_err) {
			*out_err = state.last_error;
			parse_error_fallback(out_err, status, &scan_err);
		}
		for (size_t i = 0; i < state.error_count; i++) {
			gtext_yaml_error_free(&state.errors[i]);
		}
		gtext_allocator_free(opts->allocator, state.errors);
		gtext_allocator_free(opts->allocator, state.top_nodes);
		yaml_context_free(state.ctx);
		return status;
	}

	GTEXT_YAML_Node *root = NULL;
	if (state.top_count == 1) {
		root = state.top_nodes[0];
	} else if (state.top_count > 1) {
		root = yaml_node_new_sequence(state.ctx, state.top_count, NULL, NULL);
		if (!root) {
			parser_free(&state.parser);
			for (size_t i = 0; i < state.error_count; i++) {
				gtext_yaml_error_free(&state.errors[i]);
			}
			gtext_allocator_free(opts->allocator, state.errors);
			gtext_allocator_free(opts->allocator, state.top_nodes);
			yaml_context_free(state.ctx);
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "Out of memory creating recovery root";
			}
			return GTEXT_YAML_E_OOM;
		}
		for (size_t i = 0; i < state.top_count; i++) {
			root->as.sequence.children[i] = state.top_nodes[i];
		}
		root->as.sequence.count = state.top_count;
	}

	state.doc->root = root;

	if (root) {
		GTEXT_YAML_Error resolve_error = {0};
		GTEXT_YAML_Status resolve_status = yaml_resolve_document(state.doc, &resolve_error);
		if (resolve_status != GTEXT_YAML_OK) {
			if (resolve_status == GTEXT_YAML_E_OOM || resolve_status == GTEXT_YAML_E_LIMIT) {
				parser_free(&state.parser);
				for (size_t i = 0; i < state.error_count; i++) {
					gtext_yaml_error_free(&state.errors[i]);
				}
				gtext_allocator_free(opts->allocator, state.errors);
				gtext_allocator_free(opts->allocator, state.top_nodes);
				yaml_context_free(state.ctx);
				if (out_err) {
					*out_err = resolve_error;
					if (out_err->code == GTEXT_YAML_OK) {
						out_err->code = resolve_status;
						out_err->message = "Out of memory resolving document";
					}
				}
				return resolve_status;
			}
			if (resolve_error.code == GTEXT_YAML_OK) {
				partial_error_fallback(&resolve_error, NULL, resolve_status);
			}
			if (!partial_errors_push(&state, &resolve_error)) {
				parser_free(&state.parser);
				for (size_t i = 0; i < state.error_count; i++) {
					gtext_yaml_error_free(&state.errors[i]);
				}
				gtext_allocator_free(opts->allocator, state.errors);
				gtext_allocator_free(opts->allocator, state.top_nodes);
				yaml_context_free(state.ctx);
				if (out_err) {
					out_err->code = GTEXT_YAML_E_OOM;
					out_err->message = "Out of memory storing resolve error";
				}
				return GTEXT_YAML_E_OOM;
			}
		}
	}

	/* After yaml_resolve_document(), not before: a merge key expands into a
	   mapping node that resolution creates (yaml_resolve.c), so a count taken
	   above misses it.  Here rather than inside the `if (root)` above because a
	   resolve error this parser *recovers* from still leaves the nodes it made
	   in the tree, and an empty document - no root - has a real count of zero
	   rather than no count at all. */
	state.doc->node_count = state.ctx->node_count;

	parser_free(&state.parser);
	gtext_allocator_free(opts->allocator, state.top_nodes);

	*out_doc = state.doc;
	*out_errors = state.errors;
	*out_error_count = state.error_count;

	return GTEXT_YAML_OK;
}

/* ============================================================================
 * Multi-Document Parser (gtext_yaml_parse_all)
 * ============================================================================ */

/**
 * @brief State for multi-document parsing.
 */
typedef struct {
	GTEXT_YAML_Document **documents;    /* Array of parsed documents */
	size_t count;                        /* Number of documents parsed */
	size_t capacity;                     /* Capacity of documents array */
	
	parser_state *current_parser;        /* Current document parser */
	yaml_context *current_context;       /* Current document context */
	GTEXT_YAML_Document *current_doc;    /* Current document being built */
	size_t current_doc_index;            /* Index of current document */
	
	const GTEXT_YAML_Parse_Options *options;
	GTEXT_YAML_Error *error;
	bool failed;
	/* Whether the open document exists only because a directive needed
	 * somewhere to be recorded. A '%' line has to open a document, but the
	 * directive is a prologue to the document that follows rather than one of
	 * its own - without this, "%YAML 1.2" over "--- text" produced a null
	 * document in front of the real one. A '---' adopts that document and
	 * clears the flag; if the stream ends with it still set, the directive
	 * had no document to apply to. */
	bool current_from_directive;
} multidoc_state;

/**
 * @brief Finalize the current document and add it to the array.
 */
static bool multidoc_finalize_document(multidoc_state *state) {
	parser_state *p = NULL;

	if (!state->current_parser || !state->current_doc) {
		return true;  /* Nothing to finalize */
	}
	
	/* Finalize any open block collections */
	p = state->current_parser;
	if (close_block_contexts(p, -1) != GTEXT_YAML_OK) {
		state->failed = true;
		return false;
	}
	/* The same check gtext_yaml_parse() makes: a document that ended inside a
	 * flow collection is broken, not empty.  This path had been left out, so
	 * gtext_yaml_parse_all() still accepted what gtext_yaml_parse() refused. */
	if (check_flow_contexts_closed(p) != GTEXT_YAML_OK) {
		state->failed = true;
		return false;
	}
	if (check_outer_property_claimed(p) != GTEXT_YAML_OK) {
		state->failed = true;
		return false;
	}
	
	/* Resolve aliases */
	GTEXT_YAML_Status status = resolve_aliases(p);
	if (status != GTEXT_YAML_OK) {
		state->failed = true;
		return false;
	}
	
	/* Set document root */
	state->current_doc->root = p->root;

	if (!finalize_tag_handles(p, state->current_doc)) {
		state->failed = true;
		if (state->error) {
			state->error->code = GTEXT_YAML_E_OOM;
			state->error->message = "Out of memory finalizing tag handles";
		}
		return false;
	}

	/* Resolve tags and implicit scalar types */
	status = yaml_resolve_document(state->current_doc, state->error);
	if (status != GTEXT_YAML_OK) {
		state->failed = true;
		return false;
	}

	/* After yaml_resolve_document(), not before: a merge key expands into a
	   mapping node that resolution creates (yaml_resolve.c), so a count taken
	   above misses it.  The context holds one count per document - every path
	   that builds a document gives it a context of its own - so this is that
	   document's own total and not a running one. */
	state->current_doc->node_count = state->current_doc->ctx->node_count;
	
	/* Add to documents array */
	if (state->count >= state->capacity) {
		size_t new_capacity = state->capacity == 0 ? 4 : state->capacity * 2;
		/* allocator-exempt: the array of document pointers, and only that.
		 *
		 * gtext_yaml_parse_all()'s published contract has the caller release it
		 * with plain free() - the example in yaml_dom.h says so. Routing it
		 * through a caller's allocator would turn that documented call into a
		 * free through the wrong allocator, which is heap corruption in exactly
		 * the code that was written against the documentation. The documents
		 * themselves, which are all the memory of any size, do go through the
		 * allocator.
		 *
		 * The array is never freed through a caller's allocator anywhere, so
		 * the two still do not mix. */
		GTEXT_YAML_Document **new_docs = (GTEXT_YAML_Document **)realloc( // allocator-exempt
			state->documents, new_capacity * sizeof(GTEXT_YAML_Document *)
		);
		if (!new_docs) {
			state->failed = true;
			if (state->error) {
				state->error->code = GTEXT_YAML_E_OOM;
				state->error->message = "Out of memory growing documents array";
			}
			return false;
		}
		state->documents = new_docs;
		state->capacity = new_capacity;
	}
	
	state->documents[state->count++] = state->current_doc;
	
	/* Clean up parser state (but not context - it's owned by document) */
	parser_free(state->current_parser);
	gtext_allocator_free(state->options->allocator, state->current_parser);
	state->current_parser = NULL;
	state->current_context = NULL;
	state->current_doc = NULL;
	
	return true;
}

/**
 * @brief Start a new document in the multi-document stream.
 */
static bool multidoc_start_document(multidoc_state *state) {
	/* Create context for this document */
	yaml_context *ctx = yaml_context_new(state->options->allocator);
	if (!ctx) {
		state->failed = true;
		if (state->error) {
			state->error->code = GTEXT_YAML_E_OOM;
			state->error->message = "Out of memory creating context";
		}
		return false;
	}
	
	/* Create document */
	GTEXT_YAML_Document *doc = (GTEXT_YAML_Document *)yaml_context_alloc(
		ctx, sizeof(GTEXT_YAML_Document), 8
	);
	if (!doc) {
		yaml_context_free(ctx);
		state->failed = true;
		if (state->error) {
			state->error->code = GTEXT_YAML_E_OOM;
			state->error->message = "Out of memory creating document";
		}
		return false;
	}
	
	memset(doc, 0, sizeof(*doc));
	doc->ctx = ctx;
	doc->options = *state->options;
	doc->document_index = state->current_doc_index++;
	
	/* Initialize parser state */
	parser_state *parser = (parser_state *)gtext_allocator_malloc(state->options->allocator, sizeof(parser_state));
	if (!parser) {
		yaml_context_free(ctx);
		state->failed = true;
		if (state->error) {
			state->error->code = GTEXT_YAML_E_OOM;
			state->error->message = "Out of memory creating parser";
		}
		return false;
	}
	
	if (!parser_init(parser, ctx, state->error)) {
		gtext_allocator_free(state->options->allocator, parser);
		yaml_context_free(ctx);
		state->failed = true;
		if (state->error) {
			state->error->code = GTEXT_YAML_E_OOM;
			state->error->message = "Out of memory initializing parser";
		}
		return false;
	}
	parser->doc = doc;
	parser->document_started = true;  /* Mark as started */
	
	state->current_context = ctx;
	state->current_doc = doc;
	state->current_parser = parser;
	state->current_from_directive = false;
	
	return true;
}

/**
 * @brief Whether the open document is nothing but directives.
 *
 * A directive has to be followed by a document (6.8): "%YAML 1.2" on its own
 * is a prologue with nothing to prologue, and was being accepted as a null
 * document. The end of the stream can arrive as either DOCUMENT_END or
 * STREAM_END depending on how the input ends, so both ask.
 */
static bool multidoc_bare_directive(multidoc_state *state) {
	if (!state->current_parser || !state->current_doc) return false;
	if (!state->current_from_directive) return false;
	state->failed = true;
	if (state->error) {
		state->error->code = GTEXT_YAML_E_INVALID;
		state->error->message = "Directive with no document to apply to";
	}
	return true;
}

/**
 * @brief Event callback for multi-document parsing.
 */
static GTEXT_YAML_Status multidoc_callback(
	GTEXT_YAML_Stream *s,
	const void *event_payload,
	void *user_data
) {
	multidoc_state *state = (multidoc_state *)user_data;
	const GTEXT_YAML_Event *event = (const GTEXT_YAML_Event *)event_payload;
	GTEXT_YAML_Event_Type type = event->type;

	if (state->failed) return GTEXT_YAML_E_STATE;
	
	/* Handle document boundaries */
	if (type == GTEXT_YAML_EVENT_DOCUMENT_START) {
		/* A document opened only to hold a directive is this document, not the
		 * one before it: adopt it rather than closing it and emitting a null. */
		if (state->current_parser && state->current_from_directive) {
			state->current_from_directive = false;
			/* This branch is where the "---" that a directive prologue
			 * requires actually lands: DOCUMENT_START is handled here and
			 * deliberately not forwarded, so parse_callback never sees the
			 * marker and has to be told the wait is over. */
			if (event->explicit_marker) {
				state->current_parser->directive_awaits_marker = false;
			}
			if (state->current_doc) {
				state->current_doc->explicit_start = event->explicit_marker;
			}
			return GTEXT_YAML_OK;
		}

		/* If we already have a document started, finalize it */
		if (state->current_parser && state->current_parser->document_started) {
			if (!multidoc_finalize_document(state)) {
				return GTEXT_YAML_E_OOM;
			}
		}
		
		/* Start new document */
		if (!multidoc_start_document(state)) {
			return GTEXT_YAML_E_OOM;
		}
		state->current_doc->explicit_start = event->explicit_marker;
		
		/* Don't pass DOCUMENT_START to the single-doc parser callback */
		/* as it's already marked as started */
		return GTEXT_YAML_OK;
	}
	
	if (type == GTEXT_YAML_EVENT_DOCUMENT_END) {
		if (multidoc_bare_directive(state)) return GTEXT_YAML_E_INVALID;
		if (state->current_doc) {
			state->current_doc->explicit_end = event->explicit_marker;
		}
		/* Finalize current document */
		if (!multidoc_finalize_document(state)) {
			return GTEXT_YAML_E_OOM;
		}
		return GTEXT_YAML_OK;
	}
	
	if (type == GTEXT_YAML_EVENT_STREAM_END) {
		if (multidoc_bare_directive(state)) return GTEXT_YAML_E_INVALID;
		/* Finalize any remaining document */
		if (state->current_parser) {
			if (!multidoc_finalize_document(state)) {
				return GTEXT_YAML_E_OOM;
			}
		}
		return GTEXT_YAML_OK;
	}
	
	/* If no document started yet, start one (implicit document) */
	if (!state->current_parser) {
		if (!multidoc_start_document(state)) {
			return GTEXT_YAML_E_OOM;
		}
		/* A directive is the only event that reaches here without a document
		 * boundary ahead of it - comments produce no event and everything
		 * else is preceded by DOCUMENT_START - so this comparison is never
		 * false today and a mutation of it survives the suite. It stays
		 * because the flag has to mean what it is named if that changes. */
		state->current_from_directive = (type == GTEXT_YAML_EVENT_DIRECTIVE);
	}
	
	/* Pass event to current document's parser */
	return parse_callback(s, event_payload, state->current_parser);
}

/**
 * @brief Parse all documents in a YAML stream.
 */
GTEXT_API GTEXT_YAML_Document **gtext_yaml_parse_all(
	const char *input,
	size_t length,
	size_t *document_count,
	const GTEXT_YAML_Parse_Options *options,
	GTEXT_YAML_Error *error
) {
	if (!input) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Input string is NULL";
		}
		return NULL;
	}
	
	if (!document_count) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "document_count parameter is NULL";
		}
		return NULL;
	}
	
	GTEXT_YAML_Parse_Options effective_opts =
		gtext_yaml_parse_options_effective(options);
	const GTEXT_YAML_Parse_Options *opts = &effective_opts;
	
	/* Initialize multidoc state */
	multidoc_state state = {0};
	state.options = opts;
	state.error = error;
	
	/* Create streaming parser */
	GTEXT_YAML_Stream *stream = gtext_yaml_stream_new(opts, multidoc_callback, &state);
	if (!stream) {
		if (error) {
			error->code = GTEXT_YAML_E_OOM;
			error->message = "Out of memory creating stream parser";
		}
		return NULL;
	}
	gtext_yaml_stream_retain_decoded_input(stream);
	
	/* Enable synchronous mode so aliases can be processed immediately */
	gtext_yaml_stream_set_sync_mode(stream, true);
	
	/* Feed input to streaming parser */
	GTEXT_YAML_Status status = gtext_yaml_stream_feed(stream, input, length);
	if (status == GTEXT_YAML_OK) {
		status = gtext_yaml_stream_finish(stream);
	}
	
	GTEXT_YAML_Error scan_err = stream_take_error(stream);
	gtext_yaml_stream_free(stream);
	
	/* Finalize any remaining document (stream doesn't emit STREAM_END) */
	if (status == GTEXT_YAML_OK && !state.failed && state.current_parser) {
		if (multidoc_bare_directive(&state)) {
			status = GTEXT_YAML_E_INVALID;
		}
		else if (!multidoc_finalize_document(&state)) {
			status = GTEXT_YAML_E_OOM;
		}
	}
	
	/* Check if parsing succeeded */
	if (status != GTEXT_YAML_OK || state.failed) {
		/* Clean up any documents that were created */
		for (size_t i = 0; i < state.count; i++) {
			gtext_yaml_free(state.documents[i]);
		}
		free(state.documents); // allocator-exempt: see the growth path
		
		/* Clean up current parser if still active */
		if (state.current_parser) {
			parser_free(state.current_parser);
			gtext_allocator_free(opts->allocator, state.current_parser);
		}
		if (state.current_context) {
			yaml_context_free(state.current_context);
		}
		
		parse_error_fallback(error, status, &scan_err);
		return NULL;
	}
	
	/* A stream may legitimately hold no documents at all - an empty input,
	 * or a lone "..." (9.2, where l-document-suffix stands on its own). The
	 * array was left NULL there, which every caller reads as a failure, so
	 * both came back as parse errors. Hand back an empty array instead:
	 * callers free it with free() the same way. */
	if (state.count == 0 && !state.documents) {
		state.documents = (GTEXT_YAML_Document **)malloc( // allocator-exempt
			sizeof(GTEXT_YAML_Document *));
		if (!state.documents) {
			if (error) {
				error->code = GTEXT_YAML_E_OOM;
				error->message = "Out of memory allocating documents array";
			}
			return NULL;
		}
		state.documents[0] = NULL;
	}

	*document_count = state.count;
	return state.documents;
}

GTEXT_API GTEXT_YAML_Document * gtext_yaml_parse_json(
	const char *input,
	size_t length,
	const GTEXT_YAML_Parse_Options *options,
	GTEXT_YAML_Error *error
) {
	if (!input) {
		if (error) {
			error->code = GTEXT_YAML_E_INVALID;
			error->message = "Input string is NULL";
		}
		return NULL;
	}

	return yaml_parse_json_document_internal(input, length, options, error, true);
}

GTEXT_API GTEXT_YAML_Document **gtext_yaml_parse_all_safe(
	const char *input,
	size_t length,
	size_t *document_count,
	GTEXT_YAML_Error *error
) {
	GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_safe();
	return gtext_yaml_parse_all(input, length, document_count, &opts, error);
}
