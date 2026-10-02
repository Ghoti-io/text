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
 * @file yaml_to_json.c
 * @brief YAML to JSON conversion utility.
 *
 * Provides conversion from YAML DOM to JSON DOM for compatible documents.
 * Strictly rejects YAML-specific features incompatible with JSON by default:
 * - Anchors and aliases
 * - Non-standard tags (except basic scalars)
 * - Merge keys (<<)
 * - Complex keys in mappings
 * - Special YAML types (set, omap, pairs)
 * Options allow relaxing some of these constraints.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/json/json_dom.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/yaml/yaml_dom.h>
#include <ghoti.io/text/yaml/yaml_stream.h>

#include "yaml_internal.h"
#include "../text_number_internal.h"

/* A coerced key's name never needs more than this: the longest is a double in
   exponential form with seventeen significant digits. */
#define GTEXT_YAML_KEY_NAME_MAX 40

/**
 * @brief The JSON name a non-string mapping key coerces to.
 *
 * The name follows the key's *value*, not the way it was written. This parser
 * already reads mapping keys that way: "0x10" and "16" are one key and are
 * refused as duplicates, as are "Null" and "~", and "True" and "true". A
 * conversion that then emitted a different name for each would contradict its
 * own idea of which keys are the same, and two YAML documents that mean the
 * same thing would come out as two different JSON documents.
 *
 * Writing the key as written is the other coherent rule, and it is the one
 * this used to follow. It loses here because the conversion is not a round
 * trip: anchors, tags, comments and styles are already gone by this point and
 * a JSON name is a string, so there is no way back for the spelling to
 * matter to. Number *values* keep their lexeme where there is a round trip to
 * protect - JSON in, identical JSON out - and that is a different operation.
 *
 * A float gets the shortest spelling that reads back as the same double, so
 * one name per value however it was written, and is forced to carry a "." or
 * an exponent so that a float key and an integer key never collide by
 * accident. ".inf" and ".nan" have no JSON spelling at all and are refused,
 * as the writer already refuses them as values.
 */
static GTEXT_YAML_Status coerce_key_name(
	const GTEXT_YAML_Node *key,
	char *buf,
	size_t buf_size,
	const char **out,
	GTEXT_YAML_Error *out_err
) {
	switch (gtext_yaml_node_type(key)) {
	case GTEXT_YAML_NULL:
		*out = "null";
		return GTEXT_YAML_OK;

	case GTEXT_YAML_BOOL: {
		bool b = false;
		if (!gtext_yaml_node_as_bool(key, &b)) break;
		*out = b ? "true" : "false";
		return GTEXT_YAML_OK;
	}

	case GTEXT_YAML_INT: {
		int64_t v = 0;
		if (!gtext_yaml_node_as_int(key, &v)) break;
		if (gtext_number_format_i64(buf, buf_size, v) < 0) break;
		*out = buf;
		return GTEXT_YAML_OK;
	}

	case GTEXT_YAML_FLOAT: {
		double d = 0;
		if (!gtext_yaml_node_as_float(key, &d)) break;
		if (!isfinite(d)) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_INVALID;
				out_err->message =
					"cannot convert: an infinite or NaN mapping key has no JSON name";
			}
			return GTEXT_YAML_E_INVALID;
		}
		/* The shortest spelling that reads back as the same double, so that
		   0.1 is "0.1" rather than "0.10000000000000001" and equal values
		   still give equal names. Both halves are done in the C locale, so
		   the name carries a "." wherever this runs. */
		int n = -1;
		for (int prec = 15; prec <= 17; prec++) {
			n = gtext_number_format_double(
				buf, buf_size, d, GTEXT_NUMBER_GENERAL, prec);
			if (n < 0 || (size_t)n >= buf_size) break;
			if (gtext_number_strtod(buf, NULL) == d) break;
		}
		if (n < 0 || (size_t)n >= buf_size) break;
		/* "1e+20" is already unmistakably a float; "1" is not. */
		if (!strpbrk(buf, ".eE")) {
			const size_t len = strlen(buf);
			if (len + 3 > buf_size) break;
			memcpy(buf + len, ".0", 3);
		}
		*out = buf;
		return GTEXT_YAML_OK;
	}

	default:
		break;
	}

	if (out_err) {
		out_err->code = GTEXT_YAML_E_INVALID;
		out_err->message = "cannot convert: complex mapping key cannot be coerced";
	}
	return GTEXT_YAML_E_INVALID;
}

#define GTEXT_YAML_JSON_MAX_SAFE_INT 9007199254740991LL
#define GTEXT_YAML_JSON_MIN_SAFE_INT (-9007199254740991LL)

typedef struct {
	GTEXT_YAML_To_JSON_Options options;
	const GTEXT_YAML_Node **stack;
	size_t stack_len;
	size_t stack_cap;
	/* Budget for the number of YAML nodes this conversion may visit.
	 *
	 * Resolving aliases turns a DAG into a tree, so a document whose size is
	 * linear in the input can convert to one that is exponential in it.  The
	 * stack check above catches a cycle, but the classic alias bomb is not
	 * cyclic - every path through it is distinct - so it passed the cycle
	 * check and then allocated until malloc() failed.
	 *
	 * Taken from the document's max_alias_expansion, which is documented as
	 * the total alias-expanded node limit and was not consulted here.
	 *
	 * A document gets its options through
	 * gtext_yaml_parse_options_effective(), however it was made, and that
	 * turns a zero into the library default - so the 0 case below is reached
	 * only by a document whose options were written into the struct directly.
	 * It still means unlimited when it happens. */
	size_t nodes_visited;
	size_t max_nodes;
	/* How deep the conversion may go.
	 *
	 * convert_node() is a recursion, and until now nothing bounded its depth:
	 * what stopped it was max_nodes above, which is the *alias bomb* budget
	 * and bounds depth only as a side effect of bounding node count.  So a
	 * caller who raised max_alias_expansion - the one documented knob for
	 * "this document is big and I know it" - lost a depth protection they
	 * were never told they had, and 60000 levels took the process down.  A
	 * guard that holds for a reason unrelated to what it is holding is a
	 * guard that someone fixing something else will remove.
	 *
	 * Taken from the document's own max_depth, the way write_node() takes it
	 * (see yaml_writer.c and test-yaml-built-depth.cpp): a document carries
	 * the options it was made with, and a DOM built through the API carries
	 * the default rather than no limit at all.
	 *
	 * max_depth = SIZE_MAX still means no limit, and here that is a caller
	 * asking for the same bargain gtext_json_parse() documents for its own
	 * recursive descent - "raising the limit is choosing a number rather than
	 * removing one".  The output of this function is a JSON value, and the
	 * reader that would read it back says the same thing, so there is nowhere
	 * better for the bound to come from. */
	size_t max_depth;
	/* The allocator the walk's own stack comes from: the document's, so the
	   conversion is accounted to the same place the document is. */
	const GTEXT_Allocator *alloc;
} yaml_to_json_context;

static bool yaml_tag_is_json_compatible(const char *tag)
{
	static const char * allowed_tags[] = {
		"!!str",
		"!!int",
		"!!float",
		"!!bool",
		"!!null",
		"!!seq",
		"!!map",
		"!!timestamp",
		"!!binary",
	};

	if (!tag) {
		return true;
	}

	for (size_t i = 0; i < sizeof(allowed_tags) / sizeof(allowed_tags[0]); i++) {
		if (strcmp(tag, allowed_tags[i]) == 0) {
			return true;
		}
	}

	return false;
}

static const GTEXT_YAML_Custom_Tag *yaml_to_json_find_custom_tag(
	const GTEXT_YAML_To_JSON_Options *options,
	const char *tag
)
{
	if (!options || !tag || !options->enable_custom_tags) {
		return NULL;
	}
	if (!options->custom_tags || options->custom_tag_count == 0) {
		return NULL;
	}

	for (size_t i = 0; i < options->custom_tag_count; i++) {
		const GTEXT_YAML_Custom_Tag *handler = &options->custom_tags[i];
		if (!handler->tag) {
			continue;
		}
		if (strcmp(handler->tag, tag) == 0) {
			return handler;
		}
	}

	return NULL;
}

static GTEXT_YAML_Status yaml_to_json_validate_tags(
	const char * input,
	size_t length,
	const GTEXT_YAML_Parse_Options * parse_options,
	const GTEXT_YAML_To_JSON_Options * json_options,
	GTEXT_YAML_Error * out_err
)
{
	GTEXT_YAML_Reader * reader = NULL;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;

	reader = gtext_yaml_reader_new(parse_options);
	if (!reader) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_OOM;
			out_err->message = "out of memory creating YAML reader";
		}
		return GTEXT_YAML_E_OOM;
	}

	status = gtext_yaml_reader_feed(reader, input, length, out_err);
	if (status != GTEXT_YAML_OK) {
		gtext_yaml_reader_free(reader);
		return status;
	}

	status = gtext_yaml_reader_feed(reader, NULL, 0, out_err);
	if (status != GTEXT_YAML_OK) {
		gtext_yaml_reader_free(reader);
		return status;
	}

	for (;;) {
		GTEXT_YAML_Event event;
		status = gtext_yaml_reader_next(reader, &event, out_err);
		if (status == GTEXT_YAML_E_STATE) {
			break;
		}
		if (status != GTEXT_YAML_OK) {
			gtext_yaml_reader_free(reader);
			return status;
		}
		if (event.tag && !yaml_tag_is_json_compatible(event.tag)) {
			const GTEXT_YAML_Custom_Tag *handler = yaml_to_json_find_custom_tag(
				json_options,
				event.tag
			);
			if (handler) {
				continue;
			}
			if (out_err) {
				out_err->code = GTEXT_YAML_E_INVALID;
				out_err->message = "cannot convert: explicit tag not JSON-compatible";
				out_err->offset = event.offset;
				out_err->line = event.line;
				out_err->col = event.col;
			}
			gtext_yaml_reader_free(reader);
			return GTEXT_YAML_E_INVALID;
		}
	}

	gtext_yaml_reader_free(reader);
	return GTEXT_YAML_OK;
}

static bool yaml_to_json_stack_contains(
	const yaml_to_json_context *ctx,
	const GTEXT_YAML_Node *node
)
{
	for (size_t i = 0; i < ctx->stack_len; i++) {
		if (ctx->stack[i] == node) {
			return true;
		}
	}

	return false;
}

static bool yaml_to_json_stack_push(
	yaml_to_json_context *ctx,
	const GTEXT_YAML_Node *node,
	GTEXT_YAML_Error *out_err
)
{
	if (ctx->stack_len == ctx->stack_cap) {
		size_t new_cap = ctx->stack_cap == 0 ? 16 : ctx->stack_cap * 2;
		const GTEXT_YAML_Node **new_stack = (const GTEXT_YAML_Node **)gtext_allocator_realloc(ctx->alloc, 
			ctx->stack,
			new_cap * sizeof(*new_stack)
		);
		if (!new_stack) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory tracking YAML conversion stack";
			}
			return false;
		}
		ctx->stack = new_stack;
		ctx->stack_cap = new_cap;
	}

	ctx->stack[ctx->stack_len++] = node;
	return true;
}

static void yaml_to_json_stack_pop(yaml_to_json_context *ctx)
{
	if (ctx->stack_len > 0) {
		ctx->stack_len--;
	}
}

/**
 * @brief Check if a node has a reference (anchor/alias).
 *
 * @param n Node to check
 * @return true if node is an alias or has an anchor, false otherwise
 */
static bool node_is_alias(const GTEXT_YAML_Node * n)
{
	GTEXT_YAML_Node_Type type = gtext_yaml_node_type(n);
	return type == GTEXT_YAML_ALIAS;
}

/**
 * @brief Recursively convert a YAML node to a JSON value.
 *
 * @param yaml_node YAML node to convert
 * @param out_json Pointer to store converted JSON value
 * @param out_err Error output (may be NULL)
 * @return GTEXT_YAML_OK on success, error code otherwise
 */
static GTEXT_YAML_Status convert_node(
	const GTEXT_YAML_Node * yaml_node,
	GTEXT_JSON_Value ** out_json,
	yaml_to_json_context *ctx,
	GTEXT_YAML_Error * out_err
)
{
	GTEXT_YAML_Node_Type type = GTEXT_YAML_NULL;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;
	const GTEXT_YAML_Node * target = NULL;
	const GTEXT_YAML_Custom_Tag * handler = NULL;
	const char * tag = NULL;

	if (!yaml_node || !out_json) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "convert_node: invalid arguments";
		}
		return GTEXT_YAML_E_INVALID;
	}

	ctx->nodes_visited++;
	if (ctx->max_nodes > 0 && ctx->nodes_visited > ctx->max_nodes) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_LIMIT;
			out_err->message = "cannot convert: alias expansion limit exceeded";
		}
		return GTEXT_YAML_E_LIMIT;
	}

	/* Depth, asked as its own question rather than left to the budget above.
	   stack_len is this walk's depth already, because the cycle check pushes
	   every node it descends into. */
	if (ctx->max_depth > 0 && ctx->stack_len >= ctx->max_depth) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_DEPTH;
			out_err->message = "cannot convert: nesting depth limit exceeded";
		}
		return GTEXT_YAML_E_DEPTH;
	}

	if (yaml_to_json_stack_contains(ctx, yaml_node)) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "cannot convert: cyclic alias reference detected";
		}
		return GTEXT_YAML_E_INVALID;
	}

	/* Check for incompatible references (anchors/aliases) */
	if (node_is_alias(yaml_node)) {
		if (!ctx->options.allow_resolved_aliases) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_INVALID;
				out_err->message = "cannot convert: node is an alias reference";
			}
			return GTEXT_YAML_E_INVALID;
		}

		target = gtext_yaml_alias_target(yaml_node);
		if (!target || target == yaml_node) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_INVALID;
				out_err->message = "cannot convert: unresolved alias reference";
			}
			return GTEXT_YAML_E_INVALID;
		}

		return convert_node(target, out_json, ctx, out_err);
	}

	tag = gtext_yaml_node_tag(yaml_node);
	if (tag && ctx->options.enable_custom_tags) {
		handler = yaml_to_json_find_custom_tag(&ctx->options, tag);
		if (handler && handler->to_json) {
			status = handler->to_json(yaml_node, tag, handler->user, out_json, out_err);
			if (status == GTEXT_YAML_OK && !*out_json) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message = "custom tag JSON converter did not return a value";
				}
				return GTEXT_YAML_E_INVALID;
			}
			return status;
		}
	}

	type = gtext_yaml_node_type(yaml_node);
	if (!yaml_to_json_stack_push(ctx, yaml_node, out_err)) {
		return GTEXT_YAML_E_OOM;
	}

	switch (type) {
	case GTEXT_YAML_NULL: {
		*out_json = gtext_json_new_null();
		if (!*out_json) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory creating JSON null";
			}
			status = GTEXT_YAML_E_OOM;
			break;
		}
		status = GTEXT_YAML_OK;
		break;
	}

	case GTEXT_YAML_BOOL: {
		bool value;
		if (!gtext_yaml_node_as_bool(yaml_node, &value)) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_INVALID;
				out_err->message = "failed to extract boolean value from YAML node";
			}
			status = GTEXT_YAML_E_INVALID;
			break;
		}
		*out_json = gtext_json_new_bool(value);
		if (!*out_json) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory creating JSON boolean";
			}
			status = GTEXT_YAML_E_OOM;
			break;
		}
		status = GTEXT_YAML_OK;
		break;
	}

	case GTEXT_YAML_INT: {
		int64_t value;
		if (!gtext_yaml_node_as_int(yaml_node, &value)) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_INVALID;
				out_err->message = "failed to extract integer value from YAML node";
			}
			status = GTEXT_YAML_E_INVALID;
			break;
		}
		if (value < GTEXT_YAML_JSON_MIN_SAFE_INT || value > GTEXT_YAML_JSON_MAX_SAFE_INT) {
			switch (ctx->options.large_int_policy) {
			case GTEXT_YAML_JSON_LARGE_INT_ERROR:
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message = "cannot convert: integer exceeds JSON safe range";
				}
				status = GTEXT_YAML_E_INVALID;
				break;
			case GTEXT_YAML_JSON_LARGE_INT_STRING: {
				const char * value_str = gtext_yaml_node_as_string(yaml_node);
				char buffer[32];
				if (!value_str) {
					snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
					value_str = buffer;
				}
				*out_json = gtext_json_new_string(value_str, strlen(value_str));
				if (!*out_json) {
					if (out_err) {
						out_err->code = GTEXT_YAML_E_OOM;
						out_err->message = "out of memory creating JSON string";
					}
					status = GTEXT_YAML_E_OOM;
					break;
				}
				status = GTEXT_YAML_OK;
				break;
			}
			case GTEXT_YAML_JSON_LARGE_INT_DOUBLE:
				*out_json = gtext_json_new_number_double((double)value);
				if (!*out_json) {
					if (out_err) {
						out_err->code = GTEXT_YAML_E_OOM;
						out_err->message = "out of memory creating JSON number";
					}
					status = GTEXT_YAML_E_OOM;
					break;
				}
				status = GTEXT_YAML_OK;
				break;
			default:
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message = "invalid large integer policy";
				}
				status = GTEXT_YAML_E_INVALID;
				break;
			}
			break;
		}

		*out_json = gtext_json_new_number_i64(value);
		if (!*out_json) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory creating JSON number";
			}
			status = GTEXT_YAML_E_OOM;
			break;
		}
		status = GTEXT_YAML_OK;
		break;
	}

	case GTEXT_YAML_FLOAT: {
		double value;
		if (!gtext_yaml_node_as_float(yaml_node, &value)) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_INVALID;
				out_err->message = "failed to extract float value from YAML node";
			}
			status = GTEXT_YAML_E_INVALID;
			break;
		}
		*out_json = gtext_json_new_number_double(value);
		if (!*out_json) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory creating JSON number";
			}
			status = GTEXT_YAML_E_OOM;
			break;
		}
		status = GTEXT_YAML_OK;
		break;
	}

	case GTEXT_YAML_STRING: {
		const char * value = gtext_yaml_node_as_string(yaml_node);
		if (!value) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_INVALID;
				out_err->message = "failed to extract string value from YAML node";
			}
			status = GTEXT_YAML_E_INVALID;
			break;
		}
		*out_json = gtext_json_new_string(value, strlen(value));
		if (!*out_json) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory creating JSON string";
			}
			status = GTEXT_YAML_E_OOM;
			break;
		}
		status = GTEXT_YAML_OK;
		break;
	}

	/* !!omap and !!pairs are sequences of single-pair mappings and are
	   stored as sequences, so a JSON array is what they already are: it
	   keeps their order, and for !!pairs their duplicate keys too. */
	case GTEXT_YAML_OMAP:
	case GTEXT_YAML_PAIRS:
	case GTEXT_YAML_SEQUENCE: {
		*out_json = gtext_json_new_array();
		if (!*out_json) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory creating JSON array";
			}
			status = GTEXT_YAML_E_OOM;
			break;
		}

		size_t length = gtext_yaml_sequence_length(yaml_node);
		for (size_t i = 0; i < length; i++) {
			const GTEXT_YAML_Node * child = gtext_yaml_sequence_get(yaml_node, i);
			if (!child) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message = "failed to access sequence element";
				}
				gtext_json_free(*out_json);
				*out_json = NULL;
				status = GTEXT_YAML_E_INVALID;
				break;
			}

			GTEXT_JSON_Value * elem = NULL;
			GTEXT_YAML_Status child_status = convert_node(child, &elem, ctx, out_err);
			if (child_status != GTEXT_YAML_OK) {
				gtext_json_free(*out_json);
				*out_json = NULL;
				status = child_status;
				break;
			}

			GTEXT_JSON_Status json_status = gtext_json_array_push(*out_json, elem);
			if (json_status != GTEXT_JSON_OK) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_OOM;
					out_err->message = "failed to add element to JSON array";
				}
				gtext_json_free(*out_json);
				gtext_json_free(elem);
				*out_json = NULL;
				status = GTEXT_YAML_E_OOM;
				break;
			}
		}

		break;
	}

	/* !!set is a mapping whose values are all null, and is stored as one. */
	case GTEXT_YAML_SET:
	case GTEXT_YAML_MAPPING: {
		*out_json = gtext_json_new_object();
		if (!*out_json) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory creating JSON object";
			}
			status = GTEXT_YAML_E_OOM;
			break;
		}

		size_t length = gtext_yaml_mapping_size(yaml_node);
		for (size_t i = 0; i < length; i++) {
			const GTEXT_YAML_Node * key_node = NULL;
			const GTEXT_YAML_Node * value_node = NULL;
			const GTEXT_YAML_Node * resolved_key = NULL;
			GTEXT_YAML_Node_Type key_type = GTEXT_YAML_NULL;
			const char * key = NULL;
			GTEXT_JSON_Value * value = NULL;
			GTEXT_YAML_Status value_status = GTEXT_YAML_OK;
			GTEXT_JSON_Status json_status = GTEXT_JSON_OK;

			if (!gtext_yaml_mapping_get_at(yaml_node, i, &key_node, &value_node)) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message = "failed to access mapping pair";
				}
				gtext_json_free(*out_json);
				*out_json = NULL;
				status = GTEXT_YAML_E_INVALID;
				break;
			}

			if (!key_node || !value_node) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message = "failed to access mapping key or value";
				}
				gtext_json_free(*out_json);
				*out_json = NULL;
				status = GTEXT_YAML_E_INVALID;
				break;
			}

			resolved_key = key_node;
			if (node_is_alias(key_node)) {
				if (!ctx->options.allow_resolved_aliases) {
					if (out_err) {
						out_err->code = GTEXT_YAML_E_INVALID;
						out_err->message = "cannot convert: alias used as mapping key";
					}
					gtext_json_free(*out_json);
					*out_json = NULL;
					status = GTEXT_YAML_E_INVALID;
					break;
				}
				resolved_key = gtext_yaml_alias_target(key_node);
				if (!resolved_key || resolved_key == key_node) {
					if (out_err) {
						out_err->code = GTEXT_YAML_E_INVALID;
						out_err->message = "cannot convert: unresolved alias key";
					}
					gtext_json_free(*out_json);
					*out_json = NULL;
					status = GTEXT_YAML_E_INVALID;
					break;
				}
			}

			key_type = gtext_yaml_node_type(resolved_key);
			if (key_type != GTEXT_YAML_STRING && !ctx->options.coerce_keys_to_strings) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message = "cannot convert: JSON requires string keys in objects";
				}
				gtext_json_free(*out_json);
				*out_json = NULL;
				status = GTEXT_YAML_E_INVALID;
				break;
			}

			char key_buf[GTEXT_YAML_KEY_NAME_MAX];
			key = NULL;
			if (key_type == GTEXT_YAML_STRING) {
				key = gtext_yaml_node_as_string(resolved_key);
				if (!key) {
					if (out_err) {
						out_err->code = GTEXT_YAML_E_INVALID;
						out_err->message = "failed to extract key string from YAML mapping";
					}
					gtext_json_free(*out_json);
					*out_json = NULL;
					status = GTEXT_YAML_E_INVALID;
					break;
				}
			} else {
				GTEXT_YAML_Status named = coerce_key_name(
					resolved_key, key_buf, sizeof(key_buf), &key, out_err);
				if (named != GTEXT_YAML_OK) {
					gtext_json_free(*out_json);
					*out_json = NULL;
					status = named;
					break;
				}
			}

			/* Reject merge keys (YAML 1.1 extension not compatible with JSON) */
			if (!ctx->options.allow_merge_keys && strcmp(key, "<<") == 0) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message = "cannot convert: YAML merge keys (<<) are not compatible with JSON";
				}
				gtext_json_free(*out_json);
				*out_json = NULL;
				status = GTEXT_YAML_E_INVALID;
				break;
			}

			/* Coercion is many-to-one: the null key and the empty string
			   key are different YAML keys and both spell "", as the integer
			   1 and the string "1" both spell "1".  A JSON object cannot
			   hold the two, and gtext_json_object_put() would keep the
			   second and drop the first without saying so - which is the
			   opposite of what this library does with duplicate keys
			   everywhere else, where the default is to refuse them. */
			if (gtext_json_object_get(*out_json, key, strlen(key)) != NULL) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_INVALID;
					out_err->message =
						"cannot convert: two mapping keys coerce to one JSON name";
				}
				gtext_json_free(*out_json);
				*out_json = NULL;
				status = GTEXT_YAML_E_INVALID;
				break;
			}

			value_status = convert_node(value_node, &value, ctx, out_err);
			if (value_status != GTEXT_YAML_OK) {
				gtext_json_free(*out_json);
				*out_json = NULL;
				status = value_status;
				break;
			}

			json_status = gtext_json_object_put(*out_json, key, strlen(key), value);
			if (json_status != GTEXT_JSON_OK) {
				if (out_err) {
					out_err->code = GTEXT_YAML_E_OOM;
					out_err->message = "failed to add key-value pair to JSON object";
				}
				gtext_json_free(*out_json);
				gtext_json_free(value);
				*out_json = NULL;
				status = GTEXT_YAML_E_OOM;
				break;
			}
		}

		break;
	}

	case GTEXT_YAML_ALIAS:
		/* Already checked above, but handle explicitly for completeness */
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "cannot convert: node is an alias (anchors/aliases not supported in JSON)";
		}
		status = GTEXT_YAML_E_INVALID;
		break;

	default:
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "unknown YAML node type";
		}
		status = GTEXT_YAML_E_INVALID;
		break;
	}

	yaml_to_json_stack_pop(ctx);
	return status;
}

GTEXT_API GTEXT_YAML_To_JSON_Options gtext_yaml_to_json_options_default(void)
{
	GTEXT_YAML_To_JSON_Options options = {
		.allow_resolved_aliases = false,
		.allow_merge_keys = false,
		.coerce_keys_to_strings = false,
		.large_int_policy = GTEXT_YAML_JSON_LARGE_INT_ERROR,
		.enable_custom_tags = false,
		.custom_tags = NULL,
		.custom_tag_count = 0,
	};

	return options;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_to_json_with_options(
	const GTEXT_YAML_Document * yaml_doc,
	GTEXT_JSON_Value ** out_json,
	const GTEXT_YAML_To_JSON_Options * options,
	GTEXT_YAML_Error * out_err
)
{
	yaml_to_json_context ctx = {0};
	const GTEXT_YAML_Node * root = NULL;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;

	if (!yaml_doc || !out_json) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "gtext_yaml_to_json: invalid arguments (doc and out_json must not be NULL)";
		}
		return GTEXT_YAML_E_INVALID;
	}

	ctx.options = options ? *options : gtext_yaml_to_json_options_default();
	/* The document's allocator, so what the walk builds is accounted to the
	   same place the document is. */
	const GTEXT_Allocator *alloc = yaml_doc->ctx->alloc;
	ctx.alloc = alloc;
	/* The document keeps the options it was parsed with. */
	ctx.max_nodes = yaml_doc->options.max_alias_expansion;
	ctx.max_depth = yaml_doc->options.max_depth;

	if (gtext_yaml_document_has_merge_keys(yaml_doc) && !ctx.options.allow_merge_keys) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "cannot convert: YAML merge keys (<<) are not compatible with JSON";
		}
		gtext_allocator_free(alloc, ctx.stack);
		return GTEXT_YAML_E_INVALID;
	}

	root = gtext_yaml_document_root(yaml_doc);
	if (!root) {
		/* Empty document → JSON null */
		*out_json = gtext_json_new_null();
		if (!*out_json) {
			if (out_err) {
				out_err->code = GTEXT_YAML_E_OOM;
				out_err->message = "out of memory creating JSON null for empty document";
			}
			gtext_allocator_free(alloc, ctx.stack);
			return GTEXT_YAML_E_OOM;
		}
		gtext_allocator_free(alloc, ctx.stack);
		return GTEXT_YAML_OK;
	}

	status = convert_node(root, out_json, &ctx, out_err);
	gtext_allocator_free(alloc, ctx.stack);
	return status;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_to_json(
	const GTEXT_YAML_Document * yaml_doc,
	GTEXT_JSON_Value ** out_json,
	GTEXT_YAML_Error * out_err
)
{
	return gtext_yaml_to_json_with_options(yaml_doc, out_json, NULL, out_err);
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_to_json_with_tags(
	const char * input,
	size_t length,
	const GTEXT_YAML_Parse_Options * parse_options,
	const GTEXT_YAML_To_JSON_Options * json_options,
	GTEXT_JSON_Value ** out_json,
	GTEXT_YAML_Error * out_err
)
{
	GTEXT_YAML_Document * yaml_doc = NULL;
	GTEXT_YAML_Status status = GTEXT_YAML_OK;

	if (!input || !out_json) {
		if (out_err) {
			out_err->code = GTEXT_YAML_E_INVALID;
			out_err->message = "gtext_yaml_to_json_with_tags: invalid arguments";
		}
		return GTEXT_YAML_E_INVALID;
	}

	*out_json = NULL;

	status = yaml_to_json_validate_tags(
		input,
		length,
		parse_options,
		json_options,
		out_err
	);
	if (status != GTEXT_YAML_OK) {
		return status;
	}

	yaml_doc = gtext_yaml_parse(input, length, parse_options, out_err);
	if (!yaml_doc) {
		status = out_err ? out_err->code : GTEXT_YAML_E_INVALID;
		return status == GTEXT_YAML_OK ? GTEXT_YAML_E_INVALID : status;
	}

	status = gtext_yaml_to_json_with_options(
		yaml_doc,
		out_json,
		json_options,
		out_err
	);
	gtext_yaml_free(yaml_doc);
	return status;
}
