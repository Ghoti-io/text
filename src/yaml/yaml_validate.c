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
 *
 * Validate a YAML document against a JSON Schema, and report the failure at
 * the line the user wrote.
 *
 * The verdict is the JSON Schema engine's; nothing about validation is
 * reimplemented here. What this file is for is the *coordinates*. A JSON
 * Schema failure names a place in the JSON instance - `/spec/2/env` - and the
 * JSON instance is a document the user never wrote. Handing that back with no
 * line number sends them hunting through a file that does not have those names
 * in it, which is why "convert and then validate" is not the same feature as
 * "validate this YAML".
 *
 * So: convert, validate, and if it fails, walk the pointer the engine reported
 * back through the YAML document and report the location of the node it names.
 *
 * ## The one rule that makes the walk correct
 *
 * A mapping key has to be spelled here exactly as the conversion spelled it,
 * or a pointer taken from the converted document will not find the node it
 * came from. That is why yaml_coerce_key_name() is shared with yaml_to_json.c
 * rather than copied: two readings of "what name does this key get" would
 * drift, and the drift would present as a validation error reported at the
 * wrong line - a defect that looks like a locator bug and is a conversion
 * disagreement.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/json.h>
#include <ghoti.io/text/yaml/yaml_schema.h>

#include "yaml_internal.h"

/*
 * Resolve one RFC 6901 reference token into the next node.
 *
 * `token` has already had `~1` and `~0` undone. A sequence takes it as a
 * decimal index, a mapping as a key name; anything else has no children and
 * ends the walk.
 */
static const GTEXT_YAML_Node * yaml_pointer_step(
	const GTEXT_YAML_Node *node, const char *token, size_t token_len) {
	if (!node) return NULL;

	switch (gtext_yaml_node_type(node)) {
	case GTEXT_YAML_SEQUENCE: {
		/* Parsed here rather than with strtoul: the token is not
		   NUL-terminated, and "01" and "1" are different tokens to RFC 6901
		   even though they are the same number to strtoul - only the second
		   names an element. */
		if (token_len == 0) return NULL;
		if (token_len > 1 && token[0] == '0') return NULL;
		size_t index = 0;
		for (size_t i = 0; i < token_len; i++) {
			if (token[i] < '0' || token[i] > '9') return NULL;
			if (index > (SIZE_MAX - (size_t)(token[i] - '0')) / 10) return NULL;
			index = index * 10 + (size_t)(token[i] - '0');
		}
		if (index >= gtext_yaml_sequence_length(node)) return NULL;
		return gtext_yaml_sequence_get(node, index);
	}

	case GTEXT_YAML_MAPPING: {
		const size_t n = gtext_yaml_mapping_size(node);
		for (size_t i = 0; i < n; i++) {
			const GTEXT_YAML_Node *key = NULL;
			const GTEXT_YAML_Node *value = NULL;
			if (!gtext_yaml_mapping_get_at(node, i, &key, &value)) continue;
			if (!key) continue;

			const char *name = NULL;
			char buf[64];
			if (gtext_yaml_node_type(key) == GTEXT_YAML_STRING) {
				name = gtext_yaml_node_as_string(key);
			}
			else {
				/* The same spelling the conversion gave it, which is the only
				   reason a coerced key can be found at all. A key with no JSON
				   name could not have produced this pointer, so it is skipped
				   rather than reported. */
				if (yaml_coerce_key_name(key, buf, sizeof(buf), &name, NULL,
						NULL) != GTEXT_YAML_OK) {
					continue;
				}
			}
			if (!name) continue;
			const size_t name_len = strlen(name);
			if (name_len == token_len
					&& (token_len == 0
						|| memcmp(name, token, token_len) == 0)) {
				return value;
			}
		}
		return NULL;
	}

	default:
		return NULL;
	}
}

/*
 * Walk an RFC 6901 pointer through a YAML document.
 *
 * Returns the node it names, or NULL if the pointer does not resolve - which
 * is not a defect on its own: an alias expanded during conversion gives the
 * instance a subtree that appears once in the YAML and twice in the JSON, so
 * the second copy's pointer has no YAML node of its own.
 *
 * An alias is followed where one is met, because the conversion followed it;
 * the location then reported is the anchor's, which is the only place in the
 * file the value is actually written.
 */
static const GTEXT_YAML_Node * yaml_pointer_resolve(
	const GTEXT_YAML_Document *doc, const char *pointer) {
	const GTEXT_YAML_Node *node = gtext_yaml_document_root(doc);
	if (!pointer || !*pointer) return node;
	if (*pointer != '/') return NULL;

	const char *p = pointer;
	char token[256];
	while (*p == '/') {
		p++;
		size_t len = 0;
		while (*p && *p != '/') {
			char c = *p++;
			if (c == '~') {
				/* "~1" is "/" and "~0" is "~", and in that order: undoing them
				   the other way round turns "~01" into "~1" into "/". */
				if (*p == '1') { c = '/'; p++; }
				else if (*p == '0') { c = '~'; p++; }
				/* A lone `~` is not a legal pointer. The engine wrote this
				   one, so it cannot happen; treated as a literal rather than
				   as a reason to lose the location. */
			}
			if (len + 1 >= sizeof(token)) return NULL;
			token[len++] = c;
		}
		token[len] = '\0';

		if (node && gtext_yaml_node_type(node) == GTEXT_YAML_ALIAS) {
			node = gtext_yaml_alias_target(node);
		}
		node = yaml_pointer_step(node, token, len);
		if (!node) return NULL;
	}
	if (node && gtext_yaml_node_type(node) == GTEXT_YAML_ALIAS) {
		node = gtext_yaml_alias_target(node);
	}
	return node;
}

GTEXT_API GTEXT_YAML_Validate_Options gtext_yaml_validate_options_default(void) {
	GTEXT_YAML_Validate_Options opts;
	memset(&opts, 0, sizeof(opts));
	opts.convert = gtext_yaml_to_json_options_default();
	return opts;
}

GTEXT_API GTEXT_JSON_Schema * gtext_yaml_schema_compile(
		const GTEXT_YAML_Document *schema_doc,
		const GTEXT_YAML_Validate_Options *opts, GTEXT_YAML_Error *err) {
	if (err) memset(err, 0, sizeof(*err));
	if (!schema_doc) {
		if (err) {
			err->code = GTEXT_YAML_E_INVALID;
			err->message = "a schema document is required";
		}
		return NULL;
	}

	GTEXT_YAML_Validate_Options use = opts
		? *opts : gtext_yaml_validate_options_default();

	GTEXT_JSON_Value *as_json = NULL;
	const GTEXT_YAML_Status converted = gtext_yaml_to_json_with_options(
		schema_doc, &as_json, &use.convert, err);
	if (converted != GTEXT_YAML_OK || !as_json) {
		if (err && !err->message) {
			err->code = converted;
			err->message = "the schema document cannot be expressed as JSON";
		}
		gtext_json_free(as_json);
		return NULL;
	}

	GTEXT_JSON_Error jerr;
	memset(&jerr, 0, sizeof(jerr));
	GTEXT_JSON_Schema *schema = gtext_json_schema_compile(as_json, &jerr);
	/* Freed either way: gtext_json_schema_compile() clones what it needs, so
	   the converted value has no further use even on success. */
	gtext_json_free(as_json);
	if (!schema) {
		if (err) {
			err->code = GTEXT_YAML_E_INVALID;
			/* The engine's own message, which says which keyword is at fault.
			   It points at a literal in json_schema.c and outlives this call;
			   the snippet beside it does not, so it is not carried over. */
			err->message = jerr.message
				? jerr.message : "the schema could not be compiled";
		}
		gtext_json_error_free(&jerr);
		return NULL;
	}
	gtext_json_error_free(&jerr);
	return schema;
}

GTEXT_API GTEXT_YAML_Status gtext_yaml_validate(
		const GTEXT_YAML_Document *doc, const GTEXT_JSON_Schema *schema,
		const GTEXT_YAML_Validate_Options *opts, GTEXT_YAML_Error *err) {
	if (err) memset(err, 0, sizeof(*err));
	if (!doc || !schema) {
		if (err) {
			err->code = GTEXT_YAML_E_INVALID;
			err->message = "a document and a compiled schema are required";
		}
		return GTEXT_YAML_E_INVALID;
	}

	GTEXT_YAML_Validate_Options use = opts
		? *opts : gtext_yaml_validate_options_default();

	/* The conversion's refusals stay the conversion's. A document JSON cannot
	   represent has not been judged against the schema at all, and saying so
	   is the difference between "fix your document" and "fix your schema". */
	GTEXT_JSON_Value *instance = NULL;
	const GTEXT_YAML_Status converted = gtext_yaml_to_json_with_options(
		doc, &instance, &use.convert, err);
	if (converted != GTEXT_YAML_OK || !instance) {
		if (err && !err->message) {
			err->code = converted;
			err->message = "the document cannot be expressed as JSON";
		}
		gtext_json_free(instance);
		return converted != GTEXT_YAML_OK ? converted : GTEXT_YAML_E_INVALID;
	}

	GTEXT_JSON_Error jerr;
	memset(&jerr, 0, sizeof(jerr));
	const GTEXT_JSON_Status verdict =
		gtext_json_schema_validate(schema, instance, &jerr);

	if (verdict == GTEXT_JSON_OK) {
		gtext_json_error_free(&jerr);
		gtext_json_free(instance);
		return GTEXT_YAML_OK;
	}

	if (err) {
		err->code = verdict == GTEXT_JSON_E_SCHEMA
			? GTEXT_YAML_E_SCHEMA : GTEXT_YAML_E_INVALID;
		err->message = jerr.message
			? jerr.message : "the document does not satisfy the schema";

		/* The translation this function exists for. The pointer indexes the
		   converted instance; the node it names in the *document* is what the
		   user can look at.

		   A stack buffer, and a pointer too long for it loses the position
		   rather than reaching for the heap: 512 bytes is about sixty nested
		   names, and a document nested that deep with keys that long is not
		   one where a line number is the thing standing between the user and
		   the problem. Said in the header rather than left to be discovered. */
		char pointer[512];
		const int needed = gtext_json_schema_instance_pointer(
			instance, &jerr, pointer, sizeof(pointer));
		if (needed >= 0 && (size_t)needed < sizeof(pointer)) {
			const GTEXT_YAML_Node *at = yaml_pointer_resolve(doc, pointer);
			GTEXT_YAML_Source_Location loc;
			memset(&loc, 0, sizeof(loc));
			if (at && gtext_yaml_node_source_location(at, &loc)) {
				err->offset = loc.offset;
				err->line = loc.line;
				err->col = loc.col;
			}
		}
	}

	/* Before the instance goes: the value the error borrows points into the
	   instance this is about to free, and so does the pointer rendering above. */
	gtext_json_error_free(&jerr);
	gtext_json_free(instance);
	return err ? err->code
		: (verdict == GTEXT_JSON_E_SCHEMA
			? GTEXT_YAML_E_SCHEMA : GTEXT_YAML_E_INVALID);
}
