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
 * JSON Schema validation (core subset) implementation.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/macros.h>
#include "json_internal.h"
#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_dom.h>
#include <ghoti.io/text/json/json_pointer.h>
#include <ghoti.io/text/json/json_schema.h>
#include "metaschema/metaschema_internal.h"

/*
 * The allocator is a parameter and not a field on the node.
 *
 * Every node and every string hanging off it came from one allocator - the
 * compiling schema's - so one argument threaded down the tree says it once,
 * where a per-node copy would be a pointer per node and one more place for a
 * node built on an error path to be missing it.  Compare csv_field_buffer's
 * `alloc`, which is a field because the buffer outlives the call that grew it;
 * a node is only ever freed from a walk that started at the schema.
 */
static void json_schema_node_free(
    json_schema_node * node, const GTEXT_Allocator * alloc) {
  if (!node) {
    return;
  }

  // Free properties
  if (node->properties) {
    for (size_t i = 0; i < node->properties_count; i++) {
      gtext_allocator_free(alloc, node->properties[i].key);
      json_schema_node_free(node->properties[i].schema, alloc);
    }
    gtext_allocator_free(alloc, node->properties);
  }

  // Free required keys
  if (node->required_keys) {
    for (size_t i = 0; i < node->required_count; i++) {
      gtext_allocator_free(alloc, node->required_keys[i]);
    }
    gtext_allocator_free(alloc, node->required_keys);
  }

  gtext_allocator_free(alloc, node->dynamic_ref_name);
  json_schema_node_free(node->unevaluated_items, alloc);
  json_schema_node_free(node->unevaluated_properties, alloc);

  // Free the asserted format's name
  gtext_allocator_free(alloc, node->format_name);

  // Free items schema
  json_schema_node_free(node->items_schema, alloc);

  // Free the applicator subschemas
  if (node->all_of) {
    for (size_t i = 0; i < node->all_of_count; i++) {
      json_schema_node_free(node->all_of[i], alloc);
    }
    gtext_allocator_free(alloc, node->all_of);
  }
  if (node->any_of) {
    for (size_t i = 0; i < node->any_of_count; i++) {
      json_schema_node_free(node->any_of[i], alloc);
    }
    gtext_allocator_free(alloc, node->any_of);
  }
  if (node->one_of) {
    for (size_t i = 0; i < node->one_of_count; i++) {
      json_schema_node_free(node->one_of[i], alloc);
    }
    gtext_allocator_free(alloc, node->one_of);
  }
  json_schema_node_free(node->not_schema, alloc);
  json_schema_node_free(node->if_schema, alloc);
  json_schema_node_free(node->then_schema, alloc);
  json_schema_node_free(node->else_schema, alloc);

  /* ref_target is deliberately not freed here: it is owned by the schema's
   * registry, because a recursive or shared reference would otherwise be
   * freed once per referring node. */

  if (node->prefix_items) {
    for (size_t i = 0; i < node->prefix_items_count; i++) {
      json_schema_node_free(node->prefix_items[i], alloc);
    }
    gtext_allocator_free(alloc, node->prefix_items);
  }
  json_schema_node_free(node->additional_items, alloc);
  json_schema_node_free(node->contains_schema, alloc);
  json_schema_node_free(node->additional_properties, alloc);
  json_schema_node_free(node->property_names, alloc);

  if (node->dep_schemas) {
    for (size_t i = 0; i < node->dep_schemas_count; i++) {
      gtext_allocator_free(alloc, node->dep_schemas[i].key);
      json_schema_node_free(node->dep_schemas[i].schema, alloc);
    }
    gtext_allocator_free(alloc, node->dep_schemas);
  }

  // Free dependentRequired
  if (node->dep_required) {
    for (size_t i = 0; i < node->dep_required_count; i++) {
      gtext_allocator_free(alloc, node->dep_required[i].key);
      if (node->dep_required[i].required) {
        for (size_t j = 0; j < node->dep_required[i].required_count; j++) {
          gtext_allocator_free(alloc, node->dep_required[i].required[j]);
        }
        gtext_allocator_free(alloc, node->dep_required[i].required);
      }
    }
    gtext_allocator_free(alloc, node->dep_required);
  }

  /* The provider's compiled patterns. `regex_provider` is set on a node that
   * compiled one, and on a node asserting `"format": "regex"`, which needs
   * the engine at validation time rather than at compile time; both inner
   * checks below are for the second case. */
  if (node->regex_provider) {
    if (node->pattern_regex) {
      node->regex_provider->free_fn(
          node->regex_provider->ctx, node->pattern_regex);
    }
    for (size_t i = 0; i < node->pattern_properties_count; i++) {
      if (node->pattern_properties[i].regex) {
        node->regex_provider->free_fn(
            node->regex_provider->ctx, node->pattern_properties[i].regex);
      }
    }
  }
  if (node->pattern_properties) {
    for (size_t i = 0; i < node->pattern_properties_count; i++) {
      json_schema_node_free(node->pattern_properties[i].schema, alloc);
    }
    gtext_allocator_free(alloc, node->pattern_properties);
  }

  // Free enum values (values are in context, just free array)
  gtext_allocator_free(alloc, node->enum_values);

  // Free const value (value is in context, just clear pointer)
  // Note: const_value is freed when context is freed

  gtext_allocator_free(alloc, node);
}

static GTEXT_JSON_Status json_schema_parse_type(json_schema_node * node,
    const GTEXT_JSON_Value * type_value, GTEXT_JSON_Error * err) {
  if (type_value->type == GTEXT_JSON_STRING) {
    // Single type string
    const char * type_str;
    size_t type_len;
    if (gtext_json_get_string(type_value, &type_str, &type_len) !=
        GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Invalid type value in schema"};
      }
      return GTEXT_JSON_E_INVALID;
    }

    if (json_matches(type_str, type_len, "null")) {
      node->type_flags |= JSON_SCHEMA_TYPE_NULL;
    }
    else if (json_matches(type_str, type_len, "boolean")) {
      node->type_flags |= JSON_SCHEMA_TYPE_BOOL;
    }
    else if (json_matches(type_str, type_len, "number")) {
      node->type_flags |= JSON_SCHEMA_TYPE_NUMBER;
    }
    else if (json_matches(type_str, type_len, "integer")) {
      node->type_flags |= JSON_SCHEMA_TYPE_INTEGER;
    }
    else if (json_matches(type_str, type_len, "string")) {
      node->type_flags |= JSON_SCHEMA_TYPE_STRING;
    }
    else if (json_matches(type_str, type_len, "array")) {
      node->type_flags |= JSON_SCHEMA_TYPE_ARRAY;
    }
    else if (json_matches(type_str, type_len, "object")) {
      node->type_flags |= JSON_SCHEMA_TYPE_OBJECT;
    }
    else {
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = GTEXT_JSON_E_INVALID, .message = "Unknown type in schema"};
      }
      return GTEXT_JSON_E_INVALID;
    }
  }
  else if (type_value->type == GTEXT_JSON_ARRAY) {
    // Array of types
    size_t count = gtext_json_array_size(type_value);
    for (size_t i = 0; i < count; i++) {
      const GTEXT_JSON_Value * type_elem = gtext_json_array_get(type_value, i);
      if (!type_elem || type_elem->type != GTEXT_JSON_STRING) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Invalid type array element in schema"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      GTEXT_JSON_Status status = json_schema_parse_type(node, type_elem, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
  }
  else {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Type must be string or array of strings"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  return GTEXT_JSON_OK;
}

/*
 * The vocabularies of 2020-12, as a bitmask.
 *
 * A metaschema's `$vocabulary` says which of these a schema written against
 * it uses, and a keyword from a vocabulary that is not in use is not a
 * keyword - it is an unknown member, and unknown members are ignored. That is
 * how `{"minimum": 10}` can be inert: not because `minimum` was
 * misunderstood, but because the metaschema did not ask for the vocabulary it
 * belongs to.
 */
typedef enum {
  JSON_VOCAB_CORE = 1u,
  JSON_VOCAB_APPLICATOR = 2u,
  JSON_VOCAB_UNEVALUATED = 4u,
  JSON_VOCAB_VALIDATION = 8u,
  JSON_VOCAB_META_DATA = 16u,
  JSON_VOCAB_FORMAT_ANNOTATION = 32u,
  JSON_VOCAB_FORMAT_ASSERTION = 64u,
  JSON_VOCAB_CONTENT = 128u
} json_schema_vocab;

/* What a schema with no `$schema`, or the standard one, uses. The assertion
 * vocabulary is deliberately not here: 2020-12 defines the dialect with
 * format-annotation, and a validator that asserted by default would be
 * refusing instances the dialect calls valid. */
#define JSON_VOCAB_DEFAULT                                                     \
  (JSON_VOCAB_CORE | JSON_VOCAB_APPLICATOR | JSON_VOCAB_UNEVALUATED            \
      | JSON_VOCAB_VALIDATION | JSON_VOCAB_META_DATA                           \
      | JSON_VOCAB_FORMAT_ANNOTATION | JSON_VOCAB_CONTENT)

static const struct {
  const char * uri;
  unsigned int bit;
} json_schema_vocab_uris[] = {
    {"https://json-schema.org/draft/2020-12/vocab/core", JSON_VOCAB_CORE},
    {"https://json-schema.org/draft/2020-12/vocab/applicator",
        JSON_VOCAB_APPLICATOR},
    {"https://json-schema.org/draft/2020-12/vocab/unevaluated",
        JSON_VOCAB_UNEVALUATED},
    {"https://json-schema.org/draft/2020-12/vocab/validation",
        JSON_VOCAB_VALIDATION},
    {"https://json-schema.org/draft/2020-12/vocab/meta-data",
        JSON_VOCAB_META_DATA},
    {"https://json-schema.org/draft/2020-12/vocab/format-annotation",
        JSON_VOCAB_FORMAT_ANNOTATION},
    {"https://json-schema.org/draft/2020-12/vocab/format-assertion",
        JSON_VOCAB_FORMAT_ASSERTION},
    {"https://json-schema.org/draft/2020-12/vocab/content",
        JSON_VOCAB_CONTENT},

    /*
     * 2019-09's set.  It is not 2020-12's with the date changed:
     *
     *  - `unevaluatedItems` and `unevaluatedProperties` live in 2019-09's
     *    *applicator* vocabulary, and were only split into one of their own in
     *    2020-12, so that URI carries both bits here;
     *  - there is one `format` vocabulary rather than the annotation and
     *    assertion pair, and it is the annotation one - 2019-09 leaves
     *    asserting to the implementation, which is what
     *    GTEXT_JSON_FORMAT_ASSERT already decides;
     *  - there is no `prefixItems`, so nothing needs a bit for it.
     *
     * Without these, a 2019-09 meta-schema's `$vocabulary` named URIs this
     * table did not have and the compile was refused for requiring a
     * vocabulary "this implementation does not have" - which it does have,
     * under a different name.
     */
    {"https://json-schema.org/draft/2019-09/vocab/core", JSON_VOCAB_CORE},
    {"https://json-schema.org/draft/2019-09/vocab/applicator",
        JSON_VOCAB_APPLICATOR | JSON_VOCAB_UNEVALUATED},
    {"https://json-schema.org/draft/2019-09/vocab/validation",
        JSON_VOCAB_VALIDATION},
    {"https://json-schema.org/draft/2019-09/vocab/meta-data",
        JSON_VOCAB_META_DATA},
    {"https://json-schema.org/draft/2019-09/vocab/format",
        JSON_VOCAB_FORMAT_ANNOTATION},
    {"https://json-schema.org/draft/2019-09/vocab/content",
        JSON_VOCAB_CONTENT},
    {NULL, 0}};

/*
 * Which draft a schema is written against.
 *
 * `$schema` names a dialect, and a dialect is more than a set of
 * vocabularies: the older drafts predate `$vocabulary` entirely, and they
 * disagree with 2020-12 about what some keywords mean rather than only about
 * which exist. The numbers increase with time so that "at least this draft"
 * is a comparison.
 *
 * Back to draft-04, which is where the differences stop being differences of
 * keyword set and start being differences of meaning: it spells `$id` as `id`,
 * `exclusiveMinimum` as a boolean that modifies `minimum`, and `integer` as a
 * constraint on how a number is written. All three are implemented, because
 * implementing a rule is not the same as guessing at one - and draft-04 is
 * still what OpenAPI 2.0 and a great deal of installed tooling speaks.
 *
 * draft-03 is refused. `required` there is a boolean on each property rather
 * than a list on the object, `dependencies` holds a different shape again, and
 * `divisibleBy` is `multipleOf` under another name: that is a different
 * language rather than the same one spelled differently, and nothing in this
 * engine's structure corresponds to it.
 */
typedef enum {
  JSON_DRAFT_04 = 4,
  JSON_DRAFT_06 = 6,
  JSON_DRAFT_07 = 7,
  JSON_DRAFT_2019_09 = 2019,
  JSON_DRAFT_2020_12 = 2020
} json_schema_draft;

static const struct {
  const char * uri;
  json_schema_draft draft;
} json_schema_dialects[] = {
    {"https://json-schema.org/draft/2020-12/schema", JSON_DRAFT_2020_12},
    {"https://json-schema.org/draft/2019-09/schema", JSON_DRAFT_2019_09},
    /* The older ones are `http`, and their own `$id` carries the empty
     * fragment, so both spellings turn up in real documents. */
    {"http://json-schema.org/draft-07/schema#", JSON_DRAFT_07},
    {"http://json-schema.org/draft-07/schema", JSON_DRAFT_07},
    {"http://json-schema.org/draft-06/schema#", JSON_DRAFT_06},
    {"http://json-schema.org/draft-06/schema", JSON_DRAFT_06},
    {"http://json-schema.org/draft-04/schema#", JSON_DRAFT_04},
    {"http://json-schema.org/draft-04/schema", JSON_DRAFT_04},
    {NULL, JSON_DRAFT_2020_12}};

/* Named so that the refusal can say which draft it was, rather than "an
 * unsupported dialect".
 *
 * `http://json-schema.org/schema#` is the unversioned alias, and it is here
 * rather than mapped to a draft because it names no draft: json-schema.org has
 * answered it with whatever was current, which was draft-04 for years and is
 * 2020-12 today, so a document using it has not said what it was written
 * against. That is exactly the case this engine refuses rather than guesses -
 * and it used to be refused under the *name* draft-04, which was a claim about
 * the document that the URI does not support. */
static const struct {
  const char * uri;
  const char * name;
} json_schema_dialects_refused[] = {
    {"http://json-schema.org/draft-03/schema#", "draft-03"},
    {"http://json-schema.org/draft-03/schema", "draft-03"},
    {"http://json-schema.org/schema#", "an unversioned alias"},
    {"http://json-schema.org/schema", "an unversioned alias"},
    {NULL, NULL}};

/* The draft a dialect URI names, or 0 if this engine does not read it. */
static json_schema_draft json_schema_draft_for_uri(
    const char * uri, size_t len) {
  for (size_t i = 0; json_schema_dialects[i].uri; i++) {
    const char * known = json_schema_dialects[i].uri;
    if (strlen(known) == len && memcmp(known, uri, len) == 0) {
      return json_schema_dialects[i].draft;
    }
  }
  return 0;
}

/*
 * The draft a schema object is read as: its own `$schema`, or the one in scope.
 *
 * `$schema` is scoped to the resource that declares it, and that includes the
 * object declaring it - so an object's `$schema` decides how that object's own
 * identifier is spelled. Both the pre-pass and the compiler ask this, rather
 * than each having its own idea of when the dialect changes.
 */
static json_schema_draft json_schema_draft_in(
    const GTEXT_JSON_Value * value, json_schema_draft in_scope) {
  if (!value || value->type != GTEXT_JSON_OBJECT) {
    return in_scope;
  }
  const GTEXT_JSON_Value * dialect = gtext_json_object_get(value, "$schema", 7);
  if (!dialect || dialect->type != GTEXT_JSON_STRING) {
    return in_scope;
  }
  json_schema_draft named = json_schema_draft_for_uri(
      dialect->as.string.data, dialect->as.string.len);
  return named ? named : in_scope;
}

/*
 * The identifier `value`'s own object would carry in this draft.
 *
 * draft-04 spells it `id` and draft-06 renamed it to `$id`. Both are read only
 * in the draft that has them: an `id` in a 2020-12 document is an annotation
 * somebody wrote, and reading it as a base-URI change would move the base of a
 * document that never asked for one.
 */
static const GTEXT_JSON_Value * json_schema_identifier(
    const GTEXT_JSON_Value * value, json_schema_draft draft) {
  if (!value || value->type != GTEXT_JSON_OBJECT) {
    return NULL;
  }
  return draft <= JSON_DRAFT_04 ? gtext_json_object_get(value, "id", 2)
                                : gtext_json_object_get(value, "$id", 3);
}

/*
 * The draft each keyword was introduced in.
 *
 * A keyword that did not exist yet is not a keyword: it is an unknown member,
 * and unknown members are ignored. This is the same rule as the vocabulary
 * check below, applied along the other axis - a dialect can leave a keyword
 * out by not declaring its vocabulary, or by predating it.
 *
 * Only the keywords whose absence changes an answer are listed. The
 * annotation keywords arrived at various times too, and since this engine
 * ignores them in every draft it would make no difference.
 */
static const struct {
  const char * keyword;
  json_schema_draft since;
  /* The last draft that still has it, or 0 for one that is still current.
   * `$recursiveRef` and `$recursiveAnchor` are the reason this field exists:
   * they arrived in 2019-09 and were *removed* in 2020-12, replaced by
   * `$dynamicRef` and `$dynamicAnchor`. Without an upper bound they would be
   * read in 2020-12, where they are ordinary unknown members. */
  json_schema_draft until;
} json_schema_keyword_since[] = {
    {"prefixItems", JSON_DRAFT_2020_12, 0},
    {"$dynamicRef", JSON_DRAFT_2020_12, 0},
    {"$dynamicAnchor", JSON_DRAFT_2020_12, 0},
    {"$recursiveRef", JSON_DRAFT_2019_09, JSON_DRAFT_2019_09},
    {"$recursiveAnchor", JSON_DRAFT_2019_09, JSON_DRAFT_2019_09},
    {"unevaluatedItems", JSON_DRAFT_2019_09, 0},
    {"unevaluatedProperties", JSON_DRAFT_2019_09, 0},
    {"dependentSchemas", JSON_DRAFT_2019_09, 0},
    {"dependentRequired", JSON_DRAFT_2019_09, 0},
    {"maxContains", JSON_DRAFT_2019_09, 0},
    {"minContains", JSON_DRAFT_2019_09, 0},
    {"$anchor", JSON_DRAFT_2019_09, 0},
    {"if", JSON_DRAFT_07, 0},
    {"then", JSON_DRAFT_07, 0},
    {"else", JSON_DRAFT_07, 0},
    /* draft-06's, all four. draft-04 spells the identifier `id`, which is not
     * a keyword *after* draft-04 either - a document that carries one there has
     * written an annotation - so both spellings are bounded on both sides. */
    {"$id", JSON_DRAFT_06, 0},
    {"id", JSON_DRAFT_04, JSON_DRAFT_04},
    {"const", JSON_DRAFT_06, 0},
    {"contains", JSON_DRAFT_06, 0},
    {"propertyNames", JSON_DRAFT_06, 0},
    {NULL, JSON_DRAFT_06, 0}};

/* Which vocabulary each keyword belongs to. A keyword not listed here is one
 * this engine ignores anyway, so no lookup is needed for it. */
static const struct {
  const char * keyword;
  unsigned int vocab;
} json_schema_keyword_vocab[] = {
    {"$id", JSON_VOCAB_CORE}, {"$schema", JSON_VOCAB_CORE},
    {"$ref", JSON_VOCAB_CORE}, {"$anchor", JSON_VOCAB_CORE},
    {"$dynamicRef", JSON_VOCAB_CORE}, {"$dynamicAnchor", JSON_VOCAB_CORE},
    {"$recursiveRef", JSON_VOCAB_CORE}, {"$recursiveAnchor", JSON_VOCAB_CORE},
    {"$vocabulary", JSON_VOCAB_CORE}, {"$comment", JSON_VOCAB_CORE},
    {"$defs", JSON_VOCAB_CORE},

    {"prefixItems", JSON_VOCAB_APPLICATOR}, {"items", JSON_VOCAB_APPLICATOR},
    {"contains", JSON_VOCAB_APPLICATOR},
    {"additionalProperties", JSON_VOCAB_APPLICATOR},
    {"properties", JSON_VOCAB_APPLICATOR},
    {"patternProperties", JSON_VOCAB_APPLICATOR},
    {"dependentSchemas", JSON_VOCAB_APPLICATOR},
    {"propertyNames", JSON_VOCAB_APPLICATOR}, {"if", JSON_VOCAB_APPLICATOR},
    {"then", JSON_VOCAB_APPLICATOR}, {"else", JSON_VOCAB_APPLICATOR},
    {"allOf", JSON_VOCAB_APPLICATOR}, {"anyOf", JSON_VOCAB_APPLICATOR},
    {"oneOf", JSON_VOCAB_APPLICATOR}, {"not", JSON_VOCAB_APPLICATOR},
    /* draft-07's spellings compile to the same places, so they answer to the
     * same vocabulary. */
    {"additionalItems", JSON_VOCAB_APPLICATOR},
    {"dependencies", JSON_VOCAB_APPLICATOR},
    {"definitions", JSON_VOCAB_APPLICATOR},

    {"unevaluatedItems", JSON_VOCAB_UNEVALUATED},
    {"unevaluatedProperties", JSON_VOCAB_UNEVALUATED},

    {"type", JSON_VOCAB_VALIDATION}, {"const", JSON_VOCAB_VALIDATION},
    {"enum", JSON_VOCAB_VALIDATION}, {"multipleOf", JSON_VOCAB_VALIDATION},
    {"maximum", JSON_VOCAB_VALIDATION},
    {"exclusiveMaximum", JSON_VOCAB_VALIDATION},
    {"minimum", JSON_VOCAB_VALIDATION},
    {"exclusiveMinimum", JSON_VOCAB_VALIDATION},
    {"maxLength", JSON_VOCAB_VALIDATION},
    {"minLength", JSON_VOCAB_VALIDATION}, {"pattern", JSON_VOCAB_VALIDATION},
    {"maxItems", JSON_VOCAB_VALIDATION}, {"minItems", JSON_VOCAB_VALIDATION},
    {"uniqueItems", JSON_VOCAB_VALIDATION},
    {"maxContains", JSON_VOCAB_VALIDATION},
    {"minContains", JSON_VOCAB_VALIDATION},
    {"maxProperties", JSON_VOCAB_VALIDATION},
    {"minProperties", JSON_VOCAB_VALIDATION},
    {"required", JSON_VOCAB_VALIDATION},
    {"dependentRequired", JSON_VOCAB_VALIDATION},

    {"format", JSON_VOCAB_FORMAT_ANNOTATION | JSON_VOCAB_FORMAT_ASSERTION},

    {"contentEncoding", JSON_VOCAB_CONTENT},
    {"contentMediaType", JSON_VOCAB_CONTENT},
    {"contentSchema", JSON_VOCAB_CONTENT},
    {NULL, 0}};

/* Is this keyword in a vocabulary the schema's dialect uses? */
static int json_schema_keyword_in_use(
    unsigned int vocabularies, const char * key, size_t key_len) {
  for (size_t i = 0; json_schema_keyword_vocab[i].keyword; i++) {
    const char * name = json_schema_keyword_vocab[i].keyword;
    if (strlen(name) == key_len && memcmp(name, key, key_len) == 0) {
      return (json_schema_keyword_vocab[i].vocab & vocabularies) != 0;
    }
  }
  return 1; // not ours to gate
}

/* Does this draft have this keyword at all? */
static int json_schema_keyword_in_draft(
    json_schema_draft draft, const char * key, size_t key_len) {
  for (size_t i = 0; json_schema_keyword_since[i].keyword; i++) {
    const char * name = json_schema_keyword_since[i].keyword;
    if (strlen(name) == key_len && memcmp(name, key, key_len) == 0) {
      if (draft < json_schema_keyword_since[i].since) {
        return 0;
      }
      json_schema_draft until = json_schema_keyword_since[i].until;
      return until == 0 || draft <= until;
    }
  }
  return 1;
}

/*
 * Standard JSON Schema keywords this engine does not enforce.
 *
 * Every one of these changes which instances are valid.  Ignoring such a
 * keyword means a schema that looks like it constrains data does not, and the
 * caller has no way to find out - the failure mode this list exists to
 * prevent.  A schema using one is refused at compile time instead.
 */
static const char * const json_schema_unsupported_keywords[] = {NULL};

/*
 * `format`, `contentEncoding`, `contentMediaType` and `contentSchema` were in
 * that list and should not have been: 2020-12 defines all four as
 * annotations, so a validator that ignores them is conformant and one that
 * refuses a schema for carrying them is not.  The three content keywords are
 * ignored outright.  `format` is ignored under the default policy and
 * enforced under GTEXT_JSON_FORMAT_ASSERT, which is handled by name in
 * json_schema_compile_node() - the same shape as `pattern`, and for the same
 * reason: whether it can be enforced is a property of what the caller asked
 * for rather than of this library.
 */

/*
 * `pattern` and `patternProperties` are deliberately not in that list.
 * Whether they can be enforced is not a property of this library: it depends
 * on whether the caller supplied a regular-expression engine.  They are
 * handled by name in json_schema_compile_node(), which refuses them through
 * json_schema_reject_keyword() - the same error, so a caller cannot tell the
 * two kinds of refusal apart and does not need to - when no provider is
 * present, and compiles them when one is.
 */

static int json_schema_keyword_in(
    const char * const * list, const char * key, size_t key_len) {
  for (size_t i = 0; list[i]; i++) {
    if (json_matches(key, key_len, list[i])) {
      return 1;
    }
  }
  return 0;
}

/*
 * Report a keyword the engine cannot honor.  The message is a static string,
 * as the error contract requires, so the keyword itself travels in
 * context_snippet, which gtext_json_error_free() already owns and frees.
 */
static GTEXT_JSON_Status json_schema_reject_keyword(
    const char * key, size_t key_len, GTEXT_JSON_Error * err) {
  if (err) {
    *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA_UNSUPPORTED,
        .message = "Schema uses a standard keyword this implementation does "
                   "not enforce"};
    /* allocator-exempt: this becomes err->context_snippet, which gtext_json_error_free()
       releases - and it is handed only the error, so it has no way to learn which
       allocator made this. */
    char * name = (char *)malloc(key_len + 1);  // allocator-exempt
    if (name) {
      memcpy(name, key, key_len);
      name[key_len] = '\0';
      err->context_snippet = name;
      err->context_snippet_len = key_len;
      err->caret_offset = 0;
    }
  }
  return GTEXT_JSON_E_SCHEMA_UNSUPPORTED;
}

/*
 * State threaded through compilation.
 *
 * `schema` is here so that a `$ref` can reach the registry and the cloned
 * document; `depth` bounds a schema that nests subschemas without bound, which
 * a `$ref` cycle cannot cause but a deeply literal document can.
 */
typedef struct {
  json_context * ctx;
  const GTEXT_JSON_Schema_Options * opts;
  GTEXT_JSON_Schema * schema;
  int depth;
  /** Which vocabularies this schema's dialect uses. */
  unsigned int vocabularies;
  /**
   * Which draft the schema in scope is written against.
   *
   * Scoped the same way `base_uri` is, and for the same reason: a `$ref` can
   * reach a document written against an older draft, and that document has to
   * be read as what it says it is. A schema that declares no `$schema` inherits
   * the dialect of the resource that contains it.
   */
  json_schema_draft draft;
  /**
   * The base URI in scope, which is the nearest enclosing `$id` resolved
   * against the one outside it. Borrowed from the resource table, which
   * outlives compilation. A `$ref` is resolved against this and not against
   * the document root, which is the whole difference between a reference
   * model built on URIs and one built on JSON Pointer.
   */
  const char * base_uri;
} json_schema_compile_ctx;

#define JSON_SCHEMA_MAX_COMPILE_DEPTH 256
#define JSON_SCHEMA_MAX_VALIDATE_DEPTH 256

/* Forward declaration: the applicator helpers below compile subschemas. */
static GTEXT_JSON_Status json_schema_compile_node(json_schema_node * node,
    const GTEXT_JSON_Value * schema_doc, json_schema_compile_ctx * cc,
    GTEXT_JSON_Error * err);


/*
 * How much of a provider's compile diagnostic is kept.
 *
 * A fixed buffer rather than a returned pointer, so that ownership is not a
 * question: the provider writes into memory it does not own and does not free,
 * and this function copies what it needs before returning.  The one thing a
 * provider must not do is assume more room than it was given.
 */
#define JSON_SCHEMA_REGEX_MESSAGE_MAX 256

/*
 * Report a `pattern` or `patternProperties` regular expression the provider
 * refused.
 *
 * The provider's own message is what a schema author needs - "nothing to
 * repeat at offset 1" says more than "invalid pattern" ever can - but
 * GTEXT_JSON_Error::message is a static string by contract, so the message
 * travels in context_snippet, the way json_schema_reject_keyword() sends the
 * keyword name.  The offset the provider reported goes in `offset`: the
 * pattern is the input being compiled here, so a byte offset into it is
 * exactly what that field means.
 *
 * `(size_t)-1` is passed through rather than folded to 0.  Some refusals have
 * no position - "this pattern needs an engine whose worst case is
 * exponential" is about the whole of it - and reporting those as "at byte 0"
 * would point at a character that is not the problem.
 */
static GTEXT_JSON_Status json_schema_reject_regex(
    const char * message, size_t offset, GTEXT_JSON_Error * err) {
  if (err) {
    *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
        .message = "Schema pattern is not a valid regular expression",
        .offset = offset};
    size_t len = strlen(message);
    /* allocator-exempt: this becomes err->context_snippet, which gtext_json_error_free()
       releases - and it is handed only the error, so it has no way to learn which
       allocator made this. */
    char * copy = (char *)malloc(len + 1);  // allocator-exempt
    if (copy) {
      memcpy(copy, message, len + 1);
      err->context_snippet = copy;
      err->context_snippet_len = len;
      err->caret_offset = 0;
    }
  }
  return GTEXT_JSON_E_INVALID;
}

/*
 * Compile one regular expression through the caller's provider.
 *
 * The caller has already established that a provider exists.  `doc` is the
 * schema value the pattern came from, and its bytes outlive this call: the
 * schema owns the cloned document, and the provider is given a pointer and a
 * length rather than a C string, as the vtable's documentation says.
 */
static GTEXT_JSON_Status json_schema_compile_regex(const GTEXT_JSON_Value * doc,
    json_schema_compile_ctx * cc, void ** out_regex, GTEXT_JSON_Error * err) {
  const char * text = NULL;
  size_t text_len = 0;
  if (gtext_json_typeof(doc) != GTEXT_JSON_STRING
      || gtext_json_get_string(doc, &text, &text_len) != GTEXT_JSON_OK) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "A schema pattern must be a string"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  char message[JSON_SCHEMA_REGEX_MESSAGE_MAX];
  message[0] = '\0';
  size_t offset = (size_t)-1;
  void * regex = NULL;
  const GTEXT_JSON_Regex_Provider * provider = &cc->schema->regex_provider;
  int rc = provider->compile_fn(provider->ctx, text ? text : "", text_len,
      &regex, message, sizeof(message), &offset);
  /* A provider is not trusted to have written a terminator. */
  message[sizeof(message) - 1] = '\0';

  if (rc != 0 || !regex) {
    return json_schema_reject_regex(
        message[0] ? message
                   : "the regular-expression engine refused this pattern",
        offset, err);
  }

  *out_regex = regex;
  return GTEXT_JSON_OK;
}


/*
 * Remember one absolute URI and the schema it names.
 */
/*
 * Takes ownership of `uri` either way, and hands back the interned copy in
 * `out_interned`.
 *
 * The interned pointer is what callers must keep: a URI registered twice
 * frees the second string, and a caller that went on using the one it passed
 * in would be reading freed memory. That is not hypothetical - the scan below
 * did exactly that for a resource the resolver had already registered, which
 * is every remote document with an absolute `$id` of its own.
 */
static GTEXT_JSON_Status json_schema_add_resource(GTEXT_JSON_Schema * schema,
    char * uri, const GTEXT_JSON_Value * value, GTEXT_JSON_Error * err,
    const char ** out_interned) {
  const GTEXT_Allocator * alloc = schema->ctx->alloc;
  if (out_interned) {
    *out_interned = NULL;
  }
  if (!uri) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory resolving a schema identifier"};
    }
    return GTEXT_JSON_E_OOM;
  }
  /* A URI named twice is the same resource named twice, which is legal when
   * the two are the same schema and is not worth distinguishing when they
   * are not: the first wins, as it does everywhere else a name is bound. */
  for (size_t i = 0; i < schema->resources_count; i++) {
    if (strcmp(schema->resources[i].uri, uri) == 0) {
      gtext_allocator_free(alloc, uri);
      if (out_interned) {
        *out_interned = schema->resources[i].uri;
      }
      return GTEXT_JSON_OK;
    }
  }
  if (schema->resources_count == schema->resources_capacity) {
    size_t cap =
        schema->resources_capacity ? schema->resources_capacity * 2 : 8;
    if (cap > SIZE_MAX / sizeof(json_schema_resource)) {
      gtext_allocator_free(alloc, uri);
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = GTEXT_JSON_E_OOM, .message = "Too many schema resources"};
      }
      return GTEXT_JSON_E_OOM;
    }
    json_schema_resource * grown = (json_schema_resource *)gtext_allocator_realloc(alloc, 
        schema->resources, cap * sizeof(json_schema_resource));
    if (!grown) {
      gtext_allocator_free(alloc, uri);
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory growing the resource table"};
      }
      return GTEXT_JSON_E_OOM;
    }
    schema->resources = grown;
    schema->resources_capacity = cap;
  }
  schema->resources[schema->resources_count].uri = uri;
  schema->resources[schema->resources_count].value = value;
  schema->resources_count++;
  if (out_interned) {
    *out_interned = uri;
  }
  return GTEXT_JSON_OK;
}

/*
 * Where a subschema can appear.
 *
 * The pre-pass below needs this, because `$id` and `$anchor` only mean
 * anything in a schema position. The suite says so directly - `$id` inside an
 * `enum`, and `$id` inside a keyword nobody has heard of, are not identifiers
 * - and a pre-pass that walked every object in the document would register
 * both and then resolve references to things that are not schemas.
 */
static const char * const json_schema_subschema_keywords[] = {
    "additionalProperties", "propertyNames", "items", "contains",
    "additionalItems", "not", "if", "then", "else", "unevaluatedItems",
    "unevaluatedProperties", "contentSchema", NULL};

static const char * const json_schema_subschema_list_keywords[] = {
    "allOf", "anyOf", "oneOf", "prefixItems", NULL};

static const char * const json_schema_subschema_map_keywords[] = {
    "properties", "patternProperties", "$defs", "definitions",
    "dependentSchemas", NULL};

static size_t json_schema_resource_slot(
    const GTEXT_JSON_Schema * schema, const char * base);

static GTEXT_JSON_Status json_schema_add_dynamic_anchor(
    GTEXT_JSON_Schema * schema, size_t resource_slot, const char * name,
    size_t name_len, json_schema_node * node, const GTEXT_JSON_Value * value,
    GTEXT_JSON_Error * err);

static GTEXT_JSON_Status json_schema_scan_resources(GTEXT_JSON_Schema * schema,
    const GTEXT_JSON_Value * value, const char * base,
    json_schema_draft draft, int depth, GTEXT_JSON_Error * err);

static GTEXT_JSON_Status json_schema_scan_list(GTEXT_JSON_Schema * schema,
    const GTEXT_JSON_Value * list, const char * base,
    json_schema_draft draft, int depth, GTEXT_JSON_Error * err) {
  if (!list || list->type != GTEXT_JSON_ARRAY) {
    return GTEXT_JSON_OK;
  }
  size_t n = gtext_json_array_size(list);
  for (size_t i = 0; i < n; i++) {
    GTEXT_JSON_Status status = json_schema_scan_resources(
        schema, gtext_json_array_get(list, i), base, draft, depth + 1, err);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }
  return GTEXT_JSON_OK;
}

static GTEXT_JSON_Status json_schema_scan_map(GTEXT_JSON_Schema * schema,
    const GTEXT_JSON_Value * map, const char * base,
    json_schema_draft draft, int depth, GTEXT_JSON_Error * err) {
  if (!map || map->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_OK;
  }
  size_t n = gtext_json_object_size(map);
  for (size_t i = 0; i < n; i++) {
    GTEXT_JSON_Status status = json_schema_scan_resources(
        schema, gtext_json_object_value(map, i), base, draft, depth + 1, err);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }
  return GTEXT_JSON_OK;
}

/*
 * The URI an anchor name has inside the resource `scope` names.
 *
 * That resource's URI with the name as its fragment, replacing a fragment the
 * base already carries rather than appending to it. Returns NULL on OOM, which
 * every caller passes straight to json_schema_add_resource - it takes
 * ownership either way and reports the failure once.
 */
static char * json_schema_anchor_uri(const char * scope, const char * name,
    size_t name_len, const GTEXT_Allocator * alloc) {
  size_t scope_len = strlen(scope);
  char * uri = (char *)gtext_allocator_malloc(alloc, scope_len + name_len + 2);
  if (!uri) {
    return NULL;
  }
  const char * hash = (const char *)memchr(scope, '#', scope_len);
  size_t keep = hash ? (size_t)(hash - scope) : scope_len;
  memcpy(uri, scope, keep);
  uri[keep] = '#';
  memcpy(uri + keep + 1, name, name_len);
  uri[keep + 1 + name_len] = '\0';
  return uri;
}

/*
 * Is this `$id` draft-07's location-independent identifier?
 *
 * An `$id` holding nothing but a fragment, which is how draft-07 and draft-06
 * spell what 2019-09 calls `$anchor`: it names a place inside the resource
 * already in scope rather than starting a new one. 2019-09 onward forbid a
 * fragment in `$id` altogether, so the question is only asked before then.
 *
 * The fragment has to be a plain name. `#/definitions/x` is not one of these -
 * it is not a legal `$id` in any draft - and is left to be resolved the way it
 * always was rather than quietly given a second meaning.
 *
 * **This changes no answer today, and is here anyway.** documentation/modules/
 * JSON.md used to list this as a gap - "a name this engine will not find" - and
 * that was wrong: resolving `#name` as a base change produces `base#name`,
 * which is the same string the anchor branch registers, and RFC 3986 section
 * 5.2.2 discards a base's fragment when resolving a relative reference, so
 * everything nested below resolved against the same base either way. Measured
 * before this was written, with the branch mutated out: the suite's four
 * location-independent groups pass both ways, and so do a nested `$id` below one
 * with and without a root `$id`.
 *
 * What the coincidence rests on is another function's fragment handling. Making
 * the rule the draft states explicit is what stops that from being load-bearing,
 * and it is the branch the `$anchor` gating below is the other half of.
 */
static bool json_schema_id_is_anchor(
    json_schema_draft draft, const GTEXT_JSON_Value * id) {
  if (draft >= JSON_DRAFT_2019_09 || !id || id->type != GTEXT_JSON_STRING) {
    return false;
  }
  const char * text = id->as.string.data;
  size_t len = id->as.string.len;
  if (len < 2 || text[0] != '#') {
    return false;
  }
  return memchr(text + 1, '/', len - 1) == NULL;
}

/*
 * Find every `$id` and `$anchor` below `value`, with `base` in scope.
 *
 * Run to completion before anything compiles. A `$ref` may name a resource
 * defined later in the document than the reference itself - the suite has
 * several - so resolving identifiers as they are met would answer a forward
 * reference and a backward one differently.
 */
static GTEXT_JSON_Status json_schema_scan_resources(GTEXT_JSON_Schema * schema,
    const GTEXT_JSON_Value * value, const char * base,
    json_schema_draft draft, int depth, GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = schema->ctx->alloc;
  if (!value || value->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_OK;
  }
  if (depth > JSON_SCHEMA_MAX_COMPILE_DEPTH) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_DEPTH,
          .message = "Schema nests deeper than this implementation allows"};
    }
    return GTEXT_JSON_E_DEPTH;
  }

  /*
   * Which draft this object is read as, before anything is read from it.
   *
   * The pre-pass used to run with no dialect at all, which is why the two
   * rules below were missing: both are things the older drafts do differently,
   * and there was nothing here that could have known. `$schema` is scoped to
   * the resource that declares it exactly as the base URI is, so it is picked
   * up on the way down and inherited by everything inside.
   */
  draft = json_schema_draft_in(value, draft);

  /*
   * Before 2019-09 a schema object containing `$ref` *is* that reference: core
   * says every other keyword beside it MUST be ignored. So an `$id` there does
   * not move the base URI, an `$anchor` there names nothing, and the
   * subschemas below it are not schemas. The compiler already honoured this;
   * the pre-pass did not, and registered the `$id` anyway - which is the whole
   * of the suite's "$ref prevents a sibling $id from changing the base uri".
   *
   * 2019-09 made `$ref` an applicator like any other, so from there the
   * siblings apply and this does not fire.
   */
  if (draft < JSON_DRAFT_2019_09) {
    const GTEXT_JSON_Value * ref = gtext_json_object_get(value, "$ref", 4);
    if (ref && ref->type == GTEXT_JSON_STRING) {
      return GTEXT_JSON_OK;
    }
  }

  const char * scope = base;
  char * owned_scope = NULL;

  const GTEXT_JSON_Value * id = json_schema_identifier(value, draft);
  if (json_schema_id_is_anchor(draft, id)) {
    /*
     * draft-07 and draft-06 spell a location-independent identifier as an
     * `$id` holding only a fragment. It names a place *inside* the resource in
     * scope, so it registers like an `$anchor` and leaves the scope alone.
     * Resolving it as a base change happens to produce the same URI - `base`
     * plus `#name` - which is why a reference to one has always been found;
     * what it also did was make that string the base for everything nested
     * below, so a further `$id` there resolved against `base#name`.
     */
    char * uri = json_schema_anchor_uri(
        scope, id->as.string.data + 1, id->as.string.len - 1, alloc);
    GTEXT_JSON_Status status =
        json_schema_add_resource(schema, uri, value, err, NULL);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }
  else if (id && id->type == GTEXT_JSON_STRING) {
    owned_scope = json_uri_resolve(
        base, strlen(base), id->as.string.data, id->as.string.len, alloc);
    if (!owned_scope) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory resolving $id"};
      }
      return GTEXT_JSON_E_OOM;
    }
    /* The table owns the string, and `scope` borrows the interned copy for
     * the rest of this call - which is safe because entries are never moved
     * out of the table, only appended to it. */
    const char * interned = NULL;
    GTEXT_JSON_Status status =
        json_schema_add_resource(schema, owned_scope, value, err, &interned);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
    scope = interned;
  }

  /* `$anchor` arrived in 2019-09. In draft-07 and draft-06 it is an unknown
   * member, and registering one there would answer a `$ref` the draft says
   * finds nothing - the older spelling is the fragment-only `$id` above. */
  const GTEXT_JSON_Value * anchor =
      json_schema_keyword_in_draft(draft, "$anchor", 7)
          ? gtext_json_object_get(value, "$anchor", 7)
          : NULL;
  if (anchor && anchor->type == GTEXT_JSON_STRING) {
    GTEXT_JSON_Status status = json_schema_add_resource(schema,
        json_schema_anchor_uri(
            scope, anchor->as.string.data, anchor->as.string.len, alloc),
        value, err, NULL);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }

  /*
   * A `$dynamicAnchor` is a plain `$anchor` as well as a dynamic one: 2020-12
   * core section 8.2.2 says a `$ref` to it behaves like any other anchor
   * reference within the same resource. Registering it here costs nothing and
   * answers the references that do not need the dynamic scope at all.
   */
  const GTEXT_JSON_Value * dynamic =
      json_schema_keyword_in_draft(draft, "$dynamicAnchor", 14)
          ? gtext_json_object_get(value, "$dynamicAnchor", 14)
          : NULL;
  if (dynamic && dynamic->type == GTEXT_JSON_STRING
      && dynamic->as.string.len > 0) {
    char * uri = json_schema_anchor_uri(
        scope, dynamic->as.string.data, dynamic->as.string.len, alloc);
    GTEXT_JSON_Status status =
        json_schema_add_resource(schema, uri, value, err, NULL);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
    /* Also remembered as a dynamic anchor, with no compiled node yet. The
     * schema it names is compiled afterwards whether or not a `$ref` reaches
     * it: an anchor inside a `$defs` nothing refers to is exactly the case
     * the recursive patterns rely on, and compiling only what is reachable
     * left those out. */
    status = json_schema_add_dynamic_anchor(schema,
        json_schema_resource_slot(schema, scope), dynamic->as.string.data,
        dynamic->as.string.len, NULL, value, err);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }

  /*
   * 2019-09's `$recursiveAnchor` is the same mechanism with one anonymous
   * name: a resource either offers itself as the recursive target or does
   * not, and a `$recursiveRef` asks for "the outermost one that does". It is
   * recorded in the same table under the empty name, which no `$dynamicAnchor`
   * can take - the guard above requires a non-empty one, and the 2020-12
   * meta-schema's `$anchor` pattern would refuse it anyway.
   *
   * Only `true` counts. `"$recursiveAnchor": false` is the default spelled
   * out, and a non-boolean is not this keyword at all.
   */
  const GTEXT_JSON_Value * recursive =
      json_schema_keyword_in_draft(draft, "$recursiveAnchor", 16)
          ? gtext_json_object_get(value, "$recursiveAnchor", 16)
          : NULL;
  if (recursive && recursive->type == GTEXT_JSON_BOOL) {
    bool on = false;
    if (gtext_json_get_bool(recursive, &on) == GTEXT_JSON_OK && on) {
      GTEXT_JSON_Status status = json_schema_add_dynamic_anchor(schema,
          json_schema_resource_slot(schema, scope), "", 0, NULL, value, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
  }

  for (size_t i = 0; json_schema_subschema_keywords[i]; i++) {
    const char * name = json_schema_subschema_keywords[i];
    const GTEXT_JSON_Value * sub =
        gtext_json_object_get(value, name, strlen(name));
    GTEXT_JSON_Status status;
    /* draft-07 spells the positional form `items: [...]`, so `items` is
     * scanned both ways; everything else has one shape. */
    if (sub && sub->type == GTEXT_JSON_ARRAY) {
      status = json_schema_scan_list(schema, sub, scope, draft, depth, err);
    }
    else {
      status = json_schema_scan_resources(
          schema, sub, scope, draft, depth + 1, err);
    }
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }
  for (size_t i = 0; json_schema_subschema_list_keywords[i]; i++) {
    const char * name = json_schema_subschema_list_keywords[i];
    GTEXT_JSON_Status status = json_schema_scan_list(schema,
        gtext_json_object_get(value, name, strlen(name)), scope, draft, depth,
        err);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }
  for (size_t i = 0; json_schema_subschema_map_keywords[i]; i++) {
    const char * name = json_schema_subschema_map_keywords[i];
    GTEXT_JSON_Status status = json_schema_scan_map(schema,
        gtext_json_object_get(value, name, strlen(name)), scope, draft, depth,
        err);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }
  /* draft-07's `dependencies` holds either a schema or a list of names. */
  const GTEXT_JSON_Value * deps =
      gtext_json_object_get(value, "dependencies", 12);
  if (deps && deps->type == GTEXT_JSON_OBJECT) {
    GTEXT_JSON_Status status =
        json_schema_scan_map(schema, deps, scope, draft, depth, err);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }
  return GTEXT_JSON_OK;
}

/*
 * The embedded meta-schema for `uri`, parsed on first use, or NULL.
 *
 * The 2020-12 dialect describes itself: a schema that wants to say "this
 * instance is a valid schema" writes `{"$ref":
 * "https://json-schema.org/draft/2020-12/schema"}`, and that reference has to
 * resolve to something before it can mean anything. The nine documents are
 * shipped with this library so that it does, without a resolver and without a
 * socket. Fetching them instead would put a network request in the middle of
 * a compile, aimed at a URI read out of the document being compiled, which is
 * the one thing the resolver seam exists to prevent.
 *
 * Embedding is safe here in a way it would not be for, say, the Unicode
 * tables: these URIs do not version. The reference model of 2020-12 rests on
 * each of them naming one fixed document, so a committed copy cannot fall
 * behind a newer one.
 *
 * The caller's resolver is asked first and wins, which is the answer to
 * "whose document is it": a caller who deliberately serves something at one
 * of these URIs - a mirror, a dialect of their own - is not overruled by a
 * copy they never asked for.
 *
 * A parse failure here is a bug in the generated file rather than anything
 * the caller did, so it is reported as such rather than as "no such schema",
 * which would send the caller looking at their `$ref`.
 */
static GTEXT_JSON_Status json_schema_embedded_document(
    json_schema_compile_ctx * cc, const char * uri, size_t uri_len,
    const GTEXT_JSON_Value ** out, GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = cc->ctx->alloc;
  *out = NULL;

  const json_metaschema_doc * doc = NULL;
  for (size_t i = 0; i < json_metaschema_doc_count; i++) {
    if (strlen(json_metaschema_docs[i].uri) == uri_len
        && memcmp(json_metaschema_docs[i].uri, uri, uri_len) == 0) {
      doc = &json_metaschema_docs[i];
      break;
    }
  }
  if (!doc) {
    return GTEXT_JSON_OK;
  }

  GTEXT_JSON_Schema * schema = cc->schema;

  /* One slot per document, so "have I parsed this one already" is an index
   * rather than a search. The root meta-schema's `allOf` reaches seven of the
   * other eight and several of those refer to each other, so meeting one
   * twice is the common case rather than the exception. */
  if (!schema->embedded) {
    schema->embedded = (GTEXT_JSON_Value **)gtext_allocator_calloc(alloc, 
        json_metaschema_doc_count, sizeof(GTEXT_JSON_Value *));
    if (!schema->embedded) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory holding an embedded meta-schema"};
      }
      return GTEXT_JSON_E_OOM;
    }
    schema->embedded_count = json_metaschema_doc_count;
  }

  size_t slot = (size_t)(doc - json_metaschema_docs);
  if (schema->embedded[slot]) {
    *out = schema->embedded[slot];
    return GTEXT_JSON_OK;
  }

  /* The document is held one source line at a time - see
   * metaschema/metaschema_internal.h for why - so it is joined here, parsed,
   * and the joined copy dropped. The parsed value is what is cached; the bytes
   * are wanted once. */
  size_t len = 0;
  for (size_t i = 0; i < doc->line_count; i++) {
    len += strlen(doc->lines[i]);
  }
  char * text = (char *)gtext_allocator_malloc(alloc, len + 1);
  if (!text) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory joining an embedded meta-schema"};
    }
    return GTEXT_JSON_E_OOM;
  }
  size_t at = 0;
  for (size_t i = 0; i < doc->line_count; i++) {
    size_t n = strlen(doc->lines[i]);
    memcpy(text + at, doc->lines[i], n);
    at += n;
  }
  text[len] = '\0';

  /*
   * The parse gets this schema's allocator too.
   *
   * check-allocators cannot see this line: it greps each listed file for a
   * direct malloc, and a document parsed into the C library through a call to
   * another file is not one.  Nine meta-schema documents went there - the whole
   * 2020-12 dialect, every time a schema validated another schema - with the
   * gate green and nothing in this file to find.  The same shape as
   * json_pull_reader.c calling the un-threaded stream.
   */
  GTEXT_JSON_Parse_Options popts = gtext_json_parse_options_default();
  popts.allocator = alloc;
  GTEXT_JSON_Error perr;
  memset(&perr, 0, sizeof(perr));
  GTEXT_JSON_Value * parsed = gtext_json_parse(text, len, &popts, &perr);
  gtext_allocator_free(alloc, text);
  if (!parsed) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "An embedded meta-schema did not parse"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  schema->embedded[slot] = parsed;
  *out = parsed;
  return GTEXT_JSON_OK;
}

/*
 * The resource URI a value in the table is the root of, or NULL.
 *
 * Anchors live in the same table under a URI carrying a fragment, and a
 * fragment names a place inside a resource rather than a resource, so they are
 * not answers to this question. Every resource URI is fragmentless, the root's
 * included - it is built with json_uri_without_fragment.
 */
static const char * json_schema_resource_uri_of(
    const GTEXT_JSON_Schema * schema, const GTEXT_JSON_Value * value) {
  for (size_t i = 0; i < schema->resources_count; i++) {
    if (schema->resources[i].value == value
        && strchr(schema->resources[i].uri, '#') == NULL) {
      return schema->resources[i].uri;
    }
  }
  return NULL;
}

/*
 * The base URI in scope at the end of a JSON pointer walked from `root`.
 *
 * A pointer fragment is resolved against the document, and a document may hold
 * embedded resources: `#/definitions/baz/definitions/bar`, where `baz` carries
 * an `$id`, names a subschema that belongs to `baz`'s resource and not to the
 * one the reference was written in. Compiling it in the referrer's scope makes
 * a relative `$ref` inside it resolve against the wrong document - the suite's
 * draft7 and draft6 "base URI change - change folder in subschema", which was
 * refused rather than answered wrongly only because the wrong URI happened not
 * to exist.
 *
 * The innermost enclosing resource wins, so the walk goes from the longest
 * prefix down. Each prefix is handed to gtext_json_pointer_get rather than
 * stepped through by hand, so `~0` and `~1` are unescaped by the one piece of
 * code that knows how.
 */
static const char * json_schema_pointer_base(const GTEXT_JSON_Schema * schema,
    const GTEXT_JSON_Value * root, const char * pointer, size_t len) {
  const GTEXT_Allocator * alloc = schema->ctx->alloc;
  while (len > 0) {
    /* The last `/` in the first `len` bytes. `memrchr` would say this in one
     * call and is a GNU extension, which this translation unit is not built
     * with. */
    size_t cut = 0;
    for (size_t i = len; i > 0; i--) {
      if (pointer[i - 1] == '/') {
        cut = i - 1;
        break;
      }
    }
    if (cut == 0) {
      break;
    }
    len = cut;
    /* _with_allocator, because evaluating a pointer allocates a token buffer
       and this one is evaluated once per path segment while resolving a
       `$ref`.  gtext_json_pointer_get() would put every one of them in the C
       library, and check-allocators cannot see a call. */
    const GTEXT_JSON_Value * at =
        gtext_json_pointer_get_with_allocator(root, pointer, len, alloc);
    const char * uri = at ? json_schema_resource_uri_of(schema, at) : NULL;
    if (uri) {
      return uri;
    }
  }
  return NULL;
}

/*
 * Find the schema a resolved absolute URI names.
 *
 * Two steps, because a URI is a resource and a location inside it. The
 * resource is looked up whole - the table holds anchors under their full URI,
 * so an anchor reference finds its entry directly - and a pointer fragment is
 * then walked from the resource's root.
 *
 * `base_out`, when the caller passes one, receives the base URI the target
 * compiles in if the walk crossed into an embedded resource, and is left alone
 * otherwise.
 */
static const GTEXT_JSON_Value * json_schema_find_target(
    json_schema_compile_ctx * cc, const char * uri, GTEXT_JSON_Error * err,
    const char ** base_out) {
  const GTEXT_Allocator * alloc = cc->ctx->alloc;
  GTEXT_JSON_Schema * schema = cc->schema;
  size_t uri_len = strlen(uri);

  for (size_t i = 0; i < schema->resources_count; i++) {
    if (strcmp(schema->resources[i].uri, uri) == 0) {
      return schema->resources[i].value;
    }
  }

  const char * hash = (const char *)memchr(uri, '#', uri_len);
  size_t base_len = hash ? (size_t)(hash - uri) : uri_len;
  const char * fragment = hash ? hash + 1 : "";
  size_t fragment_len = hash ? uri_len - base_len - 1 : 0;

  const GTEXT_JSON_Value * root = NULL;
  for (size_t i = 0; i < schema->resources_count; i++) {
    if (strlen(schema->resources[i].uri) == base_len
        && memcmp(schema->resources[i].uri, uri, base_len) == 0) {
      root = schema->resources[i].value;
      break;
    }
  }
  if (!root) {
    /* Not in this document. Two places it can come from: the caller's
     * resolver, and the meta-schemas this library embeds. The resolver is
     * asked first, so that a caller who deliberately serves one of those URIs
     * is not overruled by a copy they did not ask for. Without either, the
     * reference does not resolve. */
    char * without = json_uri_without_fragment(uri, uri_len, alloc);
    if (!without) {
      return NULL;
    }
    const GTEXT_JSON_Value * doc = NULL;
    if (cc->opts->resolver && cc->opts->resolver->get_fn) {
      doc = cc->opts->resolver->get_fn(
          cc->opts->resolver->ctx, without, base_len);
    }
    if (!doc
        && json_schema_embedded_document(cc, without, base_len, &doc, err)
            != GTEXT_JSON_OK) {
      gtext_allocator_free(alloc, without);
      return NULL;
    }
    if (doc) {
      /* Scanned with the URI it was asked for as its base - not the one
       * that was asked about, which still carries the fragment - so that an
       * `$id` inside it is resolved relative to where it came from. */
      const char * interned = NULL;
      GTEXT_JSON_Status status =
          json_schema_add_resource(schema, without, doc, err, &interned);
      if (status == GTEXT_JSON_OK && interned) {
        status = json_schema_scan_resources(
            schema, doc, interned, cc->draft, 0, err);
      }
      if (status != GTEXT_JSON_OK) {
        return NULL;
      }
      return json_schema_find_target(cc, uri, err, base_out);
    }
    gtext_allocator_free(alloc, without);
    return NULL;
  }
  if (fragment_len == 0) {
    return root;
  }
  if (fragment[0] != '/') {
    return NULL; // an anchor name the pre-pass did not find
  }
  /* A pointer in a fragment is percent-decoded before it is read as a
   * pointer (RFC 6901 section 6), so `%25` is a per-cent sign here and `~1`
   * is a slash in the step after. */
  size_t decoded_len = 0;
  char * decoded =
      json_uri_percent_decode(fragment, fragment_len, &decoded_len, alloc);
  if (!decoded) {
    return NULL;
  }
  const GTEXT_JSON_Value * found =
      gtext_json_pointer_get_with_allocator(root, decoded, decoded_len, alloc);
  if (found && base_out) {
    const char * inner = json_schema_resource_uri_of(schema, found);
    if (!inner) {
      inner = json_schema_pointer_base(schema, root, decoded, decoded_len);
    }
    if (inner) {
      *base_out = inner;
    }
  }
  gtext_allocator_free(alloc, decoded);
  return found;
}

/*
 * Resolve a `$ref` to a compiled node, compiling the target on first use.
 *
 * `$ref` is a URI-reference (2020-12 core section 8.2.3.1), resolved against
 * the base URI in scope rather than against the document root. This used to
 * accept only "#" and "#/...", which is the shape a reference takes when
 * nothing in the document carries an `$id` - the common case, and not the
 * rule. A schema with an `$id` anywhere in it either failed to compile or,
 * worse, resolved a pointer against the wrong resource.
 *
 * The entry is registered before the target's children compile, so a schema
 * that refers to itself terminates. Targets are owned by the registry, so one
 * reached from several places is compiled once and freed once.
 */
static GTEXT_JSON_Status json_schema_resolve_ref(json_schema_node ** out,
    const char * ref, size_t ref_len, json_schema_compile_ctx * cc,
    GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = cc->ctx->alloc;
  GTEXT_JSON_Schema * schema = cc->schema;
  char * uri = json_uri_resolve(
      cc->base_uri, strlen(cc->base_uri), ref, ref_len, alloc);
  if (!uri) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory resolving $ref"};
    }
    return GTEXT_JSON_E_OOM;
  }

  for (size_t i = 0; i < schema->refs_count; i++) {
    if (strcmp(schema->refs[i].uri, uri) == 0) {
      gtext_allocator_free(alloc, uri);
      *out = schema->refs[i].node;
      return GTEXT_JSON_OK;
    }
  }

  const char * pointer_base = NULL;
  const GTEXT_JSON_Value * target =
      json_schema_find_target(cc, uri, err, &pointer_base);
  if (!target) {
    if (err && err->code == GTEXT_JSON_OK) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
          .message = "$ref does not resolve to a schema"};
    }
    else if (err && !err->message) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
          .message = "$ref does not resolve to a schema"};
    }
    gtext_allocator_free(alloc, uri);
    return GTEXT_JSON_E_SCHEMA;
  }

  if (schema->refs_count == schema->refs_capacity) {
    size_t cap = schema->refs_capacity ? schema->refs_capacity * 2 : 8;
    if (cap > SIZE_MAX / sizeof(json_schema_ref_entry)) {
      gtext_allocator_free(alloc, uri);
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = GTEXT_JSON_E_OOM, .message = "Too many $ref targets"};
      }
      return GTEXT_JSON_E_OOM;
    }
    json_schema_ref_entry * grown = (json_schema_ref_entry *)gtext_allocator_realloc(alloc, 
        schema->refs, cap * sizeof(json_schema_ref_entry));
    if (!grown) {
      gtext_allocator_free(alloc, uri);
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory growing the $ref registry"};
      }
      return GTEXT_JSON_E_OOM;
    }
    schema->refs = grown;
    schema->refs_capacity = cap;
  }

  json_schema_node * node =
      (json_schema_node *)gtext_allocator_calloc(alloc, 1, sizeof(json_schema_node));
  if (!node) {
    gtext_allocator_free(alloc, uri);
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory allocating a $ref target"};
    }
    return GTEXT_JSON_E_OOM;
  }

  /* Registered before compiling, so a self-reference finds this entry rather
   * than recursing forever. */
  schema->refs[schema->refs_count].uri = uri;
  schema->refs[schema->refs_count].node = node;
  schema->refs_count++;

  /*
   * The target compiles in *its own* resource's scope, not the referrer's.
   * A reference that crosses into another document and then follows a
   * relative `$ref` there has to resolve it against that document's base;
   * carrying the referrer's base across the boundary is how a cross-resource
   * reference silently reads the wrong schema.
   */
  const char * saved_base = cc->base_uri;
  if (pointer_base) {
    /* The pointer crossed into an embedded resource, so the target's own
     * resource is that one rather than the one the pointer was written
     * against. */
    cc->base_uri = pointer_base;
  }
  else {
    char * target_base = json_uri_without_fragment(uri, strlen(uri), alloc);
    size_t base_slot = (size_t)-1;
    if (target_base) {
      for (size_t i = 0; i < schema->resources_count; i++) {
        if (strcmp(schema->resources[i].uri, target_base) == 0) {
          base_slot = i;
          break;
        }
      }
      gtext_allocator_free(alloc, target_base);
    }
    if (base_slot != (size_t)-1) {
      cc->base_uri = schema->resources[base_slot].uri;
    }
  }
  GTEXT_JSON_Status status = json_schema_compile_node(node, target, cc, err);
  cc->base_uri = saved_base;
  if (status != GTEXT_JSON_OK) {
    /* The entry stays in the registry so it is freed with the schema; the
     * compile as a whole is about to fail. */
    return status;
  }
  *out = node;
  return GTEXT_JSON_OK;
}

/*
 * Which vocabularies this schema's dialect uses, from its `$schema`.
 *
 * `$vocabulary` lives in the *metaschema* - the document `$schema` names -
 * and not in the schema itself, so answering this means fetching that
 * document. It arrives through the caller's resolver like any other, and when
 * there is no resolver, or it does not know the URI, the standard dialect is
 * assumed rather than the schema refused: a `$schema` naming a metaschema
 * nobody can fetch is overwhelmingly a schema written against the standard
 * dialect that said so, and refusing those would be a worse answer than
 * assuming the usual one.
 *
 * A vocabulary listed as required that this engine does not implement is the
 * one case that is refused. That is the specification's rule and it is also
 * the honest one: the metaschema has said the schema cannot be understood
 * without it.
 */
static GTEXT_JSON_Status json_schema_read_dialect(
    json_schema_compile_ctx * cc, const GTEXT_JSON_Value * doc,
    GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = cc->ctx->alloc;
  if (!doc || doc->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_OK;
  }
  const GTEXT_JSON_Value * dialect = gtext_json_object_get(doc, "$schema", 7);
  if (!dialect || dialect->type != GTEXT_JSON_STRING) {
    return GTEXT_JSON_OK; // inherit whatever is in scope
  }

  /*
   * A draft this engine knows by name. `$vocabulary` is not consulted for
   * these: 2020-12 and 2019-09 declare exactly the vocabularies this dialect
   * table records, and the drafts before them have no `$vocabulary` at all -
   * their keyword set is the draft, not a declaration inside it.
   */
  for (size_t i = 0; json_schema_dialects[i].uri; i++) {
    const char * uri = json_schema_dialects[i].uri;
    if (strlen(uri) == dialect->as.string.len
        && memcmp(uri, dialect->as.string.data, dialect->as.string.len) == 0) {
      cc->draft = json_schema_dialects[i].draft;
      cc->vocabularies = JSON_VOCAB_DEFAULT;
      return GTEXT_JSON_OK;
    }
  }

  /*
   * A draft whose keywords this engine would have to read differently rather
   * than merely ignore. Refused, which is what it does with every other
   * keyword it cannot honour: reading draft-04's boolean `exclusiveMinimum`
   * as draft-06's number does not produce a wrong keyword, it produces a
   * wrong answer about the instance.
   */
  for (size_t i = 0; json_schema_dialects_refused[i].uri; i++) {
    const char * uri = json_schema_dialects_refused[i].uri;
    if (strlen(uri) == dialect->as.string.len
        && memcmp(uri, dialect->as.string.data, dialect->as.string.len) == 0) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA_UNSUPPORTED,
            .message = "Schema is written against a draft this "
                       "implementation does not read"};
        size_t n = strlen(json_schema_dialects_refused[i].name);
        /* allocator-exempt: this becomes err->context_snippet, which gtext_json_error_free()
           releases - and it is handed only the error, so it has no way to learn which
           allocator made this. */
        char * name = (char *)malloc(n + 1);  // allocator-exempt
        if (name) {
          memcpy(name, json_schema_dialects_refused[i].name, n + 1);
          err->context_snippet = name;
          err->context_snippet_len = n;
        }
      }
      return GTEXT_JSON_E_SCHEMA_UNSUPPORTED;
    }
  }

  char * uri = json_uri_resolve(cc->base_uri, strlen(cc->base_uri),
      dialect->as.string.data, dialect->as.string.len, alloc);
  if (!uri) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory resolving $schema"};
    }
    return GTEXT_JSON_E_OOM;
  }
  /*
   * Fetched, but deliberately not registered as one of this schema's
   * resources. A metaschema is consulted, not compiled: it is full of
   * references to the published meta-schemas, and registering it put its
   * `$dynamicAnchor` into the table of anchors to compile - which then tried
   * to compile the whole of it and failed on the first `$ref` out to
   * json-schema.org. Consulting a document must not drag it in.
   */
  const GTEXT_JSON_Value * meta = NULL;
  for (size_t i = 0; i < cc->schema->resources_count; i++) {
    if (strcmp(cc->schema->resources[i].uri, uri) == 0) {
      meta = cc->schema->resources[i].value;
      break;
    }
  }
  if (!meta && cc->opts->resolver && cc->opts->resolver->get_fn) {
    meta = cc->opts->resolver->get_fn(
        cc->opts->resolver->ctx, uri, strlen(uri));
  }
  if (!meta) {
    /* The same embedded documents a `$ref` reaches. A dialect built out of a
     * subset of the standard vocabularies names one of them as its `$schema`,
     * and there is no reason that should need a resolver either. A failure to
     * parse one is reported; not finding one is not a failure, because the
     * next line treats an unknown dialect as the standard one. */
    GTEXT_JSON_Status status = json_schema_embedded_document(
        cc, uri, strlen(uri), &meta, err);
    if (status != GTEXT_JSON_OK) {
      gtext_allocator_free(alloc, uri);
      return status;
    }
  }
  gtext_allocator_free(alloc, uri);
  if (!meta || meta->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_OK;
  }
  const GTEXT_JSON_Value * vocab =
      gtext_json_object_get(meta, "$vocabulary", 11);
  if (!vocab || vocab->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_OK;
  }

  /* Core is always in use: a metaschema that left it out could not say so. */
  unsigned int mask = JSON_VOCAB_CORE;
  size_t count = gtext_json_object_size(vocab);
  for (size_t i = 0; i < count; i++) {
    size_t key_len = 0;
    const char * key = gtext_json_object_key(vocab, i, &key_len);
    const GTEXT_JSON_Value * required = gtext_json_object_value(vocab, i);
    if (!key) {
      continue;
    }
    unsigned int bit = 0;
    for (size_t k = 0; json_schema_vocab_uris[k].uri; k++) {
      if (strlen(json_schema_vocab_uris[k].uri) == key_len
          && memcmp(json_schema_vocab_uris[k].uri, key, key_len) == 0) {
        bit = json_schema_vocab_uris[k].bit;
        break;
      }
    }
    if (bit != 0) {
      mask |= bit;
      continue;
    }
    bool is_required = false;
    if (required) {
      gtext_json_get_bool(required, &is_required);
    }
    if (is_required) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA_UNSUPPORTED,
            .message = "The metaschema requires a vocabulary this "
                       "implementation does not have"};
        /* allocator-exempt: this becomes err->context_snippet, which gtext_json_error_free()
           releases - and it is handed only the error, so it has no way to learn which
           allocator made this. */
        char * name = (char *)malloc(key_len + 1);  // allocator-exempt
        if (name) {
          memcpy(name, key, key_len);
          name[key_len] = '\0';
          err->context_snippet = name;
          err->context_snippet_len = key_len;
        }
      }
      return GTEXT_JSON_E_SCHEMA_UNSUPPORTED;
    }
  }
  cc->vocabularies = mask;
  /* A dialect assembled out of 2020-12's vocabularies is a 2020-12 dialect;
   * the `$vocabulary` keyword itself does not exist before 2019-09, so
   * anything that has one is at least that new. */
  cc->draft = JSON_DRAFT_2020_12;
  return GTEXT_JSON_OK;
}

/* Compile one subschema into a freshly allocated node. */
static GTEXT_JSON_Status json_schema_compile_sub(json_schema_node ** out,
    const GTEXT_JSON_Value * doc, json_schema_compile_ctx * cc,
    GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = cc->ctx->alloc;
  json_schema_node * sub =
      (json_schema_node *)gtext_allocator_calloc(alloc, 1, sizeof(json_schema_node));
  if (!sub) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory allocating subschema"};
    }
    return GTEXT_JSON_E_OOM;
  }
  GTEXT_JSON_Status status = json_schema_compile_node(sub, doc, cc, err);
  if (status != GTEXT_JSON_OK) {
    json_schema_node_free(sub, alloc);
    return status;
  }
  *out = sub;
  return GTEXT_JSON_OK;
}

/* Compile an array of subschemas, as allOf, anyOf and oneOf all take. */
static GTEXT_JSON_Status json_schema_compile_sub_list(
    json_schema_node *** out_list, size_t * out_count, const char * keyword,
    const GTEXT_JSON_Value * value, json_schema_compile_ctx * cc,
    GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = cc->ctx->alloc;
  if (value->type != GTEXT_JSON_ARRAY) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Applicator keyword must be an array of schemas"};
    }
    (void)keyword;
    return GTEXT_JSON_E_INVALID;
  }
  size_t n = gtext_json_array_size(value);
  if (n == 0) {
    /* An empty array asserts nothing; JSON Schema requires at least one
     * element, so treat it as a malformed schema rather than a no-op. */
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Applicator keyword must have at least one schema"};
    }
    return GTEXT_JSON_E_INVALID;
  }
  if (n > SIZE_MAX / sizeof(json_schema_node *)) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = GTEXT_JSON_E_OOM, .message = "Applicator list too large"};
    }
    return GTEXT_JSON_E_OOM;
  }
  json_schema_node ** list =
      (json_schema_node **)gtext_allocator_calloc(alloc, n, sizeof(json_schema_node *));
  if (!list) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory allocating applicator list"};
    }
    return GTEXT_JSON_E_OOM;
  }
  for (size_t i = 0; i < n; i++) {
    const GTEXT_JSON_Value * elem = gtext_json_array_get(value, i);
    GTEXT_JSON_Status status =
        json_schema_compile_sub(&list[i], elem, cc, err);
    if (status != GTEXT_JSON_OK) {
      for (size_t j = 0; j < i; j++) {
        json_schema_node_free(list[j], alloc);
      }
      gtext_allocator_free(alloc, list);
      return status;
    }
  }
  *out_list = list;
  *out_count = n;
  return GTEXT_JSON_OK;
}

static GTEXT_JSON_Status json_schema_compile_body(json_schema_node * node,
    const GTEXT_JSON_Value * schema_doc, json_schema_compile_ctx * cc,
    GTEXT_JSON_Error * err);

/*
 * Compile one schema, in whatever base URI its own `$id` puts it in.
 *
 * The scope is pushed here rather than inside the body because the body
 * returns from two dozen places and every one of them would have to remember
 * to pop it. A missed pop is not a crash; it is a later `$ref` quietly
 * resolving against the wrong resource, which is the kind of wrong that shows
 * up as a validation result rather than as an error.
 */
static GTEXT_JSON_Status json_schema_compile_node(json_schema_node * node,
    const GTEXT_JSON_Value * schema_doc, json_schema_compile_ctx * cc,
    GTEXT_JSON_Error * err) {
  const char * saved = cc->base_uri;
  unsigned int saved_vocabularies = cc->vocabularies;
  json_schema_draft saved_draft = cc->draft;
  const GTEXT_Allocator * alloc = cc->ctx->alloc;
  /*
   * Here rather than in json_schema_compile_body(), which returns early for a
   * schema that is not an object - so a boolean schema, `true` or `false`,
   * used to reach validation with no owner at all.  Nothing noticed while only
   * `$dynamicRef` read this field, because a boolean schema has no keywords to
   * carry one; the per-validation marks read it for their allocator, and they
   * are allocated for any node.
   */
  node->owner = cc->schema;
  if (schema_doc && schema_doc->type == GTEXT_JSON_OBJECT) {
    const GTEXT_JSON_Value * id = json_schema_identifier(
        schema_doc, json_schema_draft_in(schema_doc, saved_draft));
    if (id && id->type == GTEXT_JSON_STRING) {
      /* The pre-pass already resolved and interned this; finding it here is
       * a lookup rather than a second resolution, so the two cannot drift. */
      char * resolved = json_uri_resolve(
          saved, strlen(saved), id->as.string.data, id->as.string.len, alloc);
      if (resolved) {
        for (size_t i = 0; i < cc->schema->resources_count; i++) {
          if (strcmp(cc->schema->resources[i].uri, resolved) == 0) {
            cc->base_uri = cc->schema->resources[i].uri;
            break;
          }
        }
        gtext_allocator_free(alloc, resolved);
      }
    }
  }
  /*
   * After the `$id`, because a relative `$schema` is resolved against the
   * base this resource establishes and not against the one outside it.
   *
   * The dialect is scoped exactly as the base URI is. A `$ref` that leaves
   * this document can land in one written against an older draft, and that
   * document has to be read as what it says it is - which is the whole
   * difference between dispatching on `$schema` and merely recording it.
   */
  GTEXT_JSON_Status status = json_schema_read_dialect(cc, schema_doc, err);
  if (status == GTEXT_JSON_OK) {
    status = json_schema_compile_body(node, schema_doc, cc, err);
  }
  cc->base_uri = saved;
  cc->vocabularies = saved_vocabularies;
  cc->draft = saved_draft;
  return status;
}

/* One plus the index of the resource `base` names, or 0 if it names none. */
static size_t json_schema_resource_slot(
    const GTEXT_JSON_Schema * schema, const char * base) {
  for (size_t i = 0; i < schema->resources_count; i++) {
    if (strcmp(schema->resources[i].uri, base) == 0) {
      return i + 1;
    }
  }
  return 0;
}

static GTEXT_JSON_Status json_schema_add_dynamic_anchor(
    GTEXT_JSON_Schema * schema, size_t resource_slot, const char * name,
    size_t name_len, json_schema_node * node, const GTEXT_JSON_Value * value,
    GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = schema->ctx->alloc;
  /* The pre-pass makes the entry with no compiled node; compilation fills it
   * in. Appending a second entry instead would leave the empty one in front
   * of it, and the search below would keep finding that. */
  for (size_t i = 0; i < schema->dynamic_anchors_count; i++) {
    json_schema_dynamic_anchor * entry = &schema->dynamic_anchors[i];
    if (entry->resource_slot == resource_slot && strlen(entry->name) == name_len
        && memcmp(entry->name, name, name_len) == 0) {
      if (node && !entry->node) {
        entry->node = node;
      }
      if (value && !entry->value) {
        entry->value = value;
      }
      return GTEXT_JSON_OK;
    }
  }
  if (schema->dynamic_anchors_count == schema->dynamic_anchors_capacity) {
    size_t cap = schema->dynamic_anchors_capacity
        ? schema->dynamic_anchors_capacity * 2
        : 4;
    json_schema_dynamic_anchor * grown =
        (json_schema_dynamic_anchor *)gtext_allocator_realloc(alloc, schema->dynamic_anchors,
            cap * sizeof(json_schema_dynamic_anchor));
    if (!grown) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory recording a $dynamicAnchor"};
      }
      return GTEXT_JSON_E_OOM;
    }
    schema->dynamic_anchors = grown;
    schema->dynamic_anchors_capacity = cap;
  }
  char * copy = (char *)gtext_allocator_malloc(alloc, name_len + 1);
  if (!copy) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory recording a $dynamicAnchor"};
    }
    return GTEXT_JSON_E_OOM;
  }
  memcpy(copy, name, name_len);
  copy[name_len] = '\0';
  schema->dynamic_anchors[schema->dynamic_anchors_count].resource_slot =
      resource_slot;
  schema->dynamic_anchors[schema->dynamic_anchors_count].name = copy;
  schema->dynamic_anchors[schema->dynamic_anchors_count].node = node;
  schema->dynamic_anchors[schema->dynamic_anchors_count].value = value;
  schema->dynamic_anchors_count++;
  return GTEXT_JSON_OK;
}

/* The schema a `$dynamicAnchor` of this name names inside this resource. */
static json_schema_node * json_schema_find_dynamic_anchor(
    const GTEXT_JSON_Schema * schema, size_t resource_slot, const char * name,
    size_t name_len) {
  for (size_t i = 0; i < schema->dynamic_anchors_count; i++) {
    const json_schema_dynamic_anchor * entry = &schema->dynamic_anchors[i];
    if (entry->resource_slot == resource_slot && strlen(entry->name) == name_len
        && memcmp(entry->name, name, name_len) == 0) {
      return entry->node;
    }
  }
  return NULL;
}

static GTEXT_JSON_Status json_schema_compile_body(json_schema_node * node,
    const GTEXT_JSON_Value * schema_doc, json_schema_compile_ctx * cc,
    GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = cc->ctx->alloc;
  if (cc->depth >= JSON_SCHEMA_MAX_COMPILE_DEPTH) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_DEPTH,
          .message = "Schema nests deeper than the compiler allows"};
    }
    return GTEXT_JSON_E_DEPTH;
  }

  /* A boolean is a schema: true accepts everything, false rejects
   * everything.  "additionalProperties": false is the common case. */
  if (schema_doc->type == GTEXT_JSON_BOOL) {
    bool b = false;
    if (gtext_json_get_bool(schema_doc, &b) != GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Invalid boolean schema"};
      }
      return GTEXT_JSON_E_INVALID;
    }
    node->is_bool_schema = 1;
    node->bool_schema_value = b ? 1 : 0;
    return GTEXT_JSON_OK;
  }

  if (schema_doc->type != GTEXT_JSON_OBJECT) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = GTEXT_JSON_E_INVALID, .message = "Schema must be an object"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  /* Which resource this schema object sits in, and any `$dynamicAnchor` it
   * declares. Both are recorded before the keywords are walked, because a
   * `$dynamicRef` in this same object has to be able to find an anchor this
   * object declares.  `owner` is set in json_schema_compile_node(), which
   * reaches a boolean schema too. */
  node->resource_slot = json_schema_resource_slot(cc->schema, cc->base_uri);
  const GTEXT_JSON_Value * dynamic_anchor =
      gtext_json_object_get(schema_doc, "$dynamicAnchor", 14);
  if (dynamic_anchor && dynamic_anchor->type == GTEXT_JSON_STRING
      && dynamic_anchor->as.string.len > 0 && node->resource_slot != 0) {
    GTEXT_JSON_Status status = json_schema_add_dynamic_anchor(cc->schema,
        node->resource_slot, dynamic_anchor->as.string.data,
        dynamic_anchor->as.string.len, node, schema_doc, err);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }
  /* 2019-09's spelling of the same thing, under the empty name. */
  const GTEXT_JSON_Value * recursive_anchor =
      gtext_json_object_get(schema_doc, "$recursiveAnchor", 16);
  if (recursive_anchor && recursive_anchor->type == GTEXT_JSON_BOOL
      && cc->draft == JSON_DRAFT_2019_09 && node->resource_slot != 0) {
    bool on = false;
    if (gtext_json_get_bool(recursive_anchor, &on) == GTEXT_JSON_OK && on) {
      GTEXT_JSON_Status status = json_schema_add_dynamic_anchor(
          cc->schema, node->resource_slot, "", 0, node, schema_doc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
  }

  /* draft-04's `type: "integer"` is about how the number is written. Recorded
   * on the node because validation is where it is asked, and the draft in scope
   * is known only here. */
  node->integer_excludes_fraction = cc->draft <= JSON_DRAFT_04 ? 1 : 0;

  /* draft-04's `exclusiveMinimum`/`exclusiveMaximum`, which are booleans over
   * `minimum`/`maximum`; -1 means the keyword was not there. */
  int draft4_exclusive_minimum = -1;
  int draft4_exclusive_maximum = -1;

  size_t obj_size = gtext_json_object_size(schema_doc);
  const int has_sibling_ref = cc->draft < JSON_DRAFT_2019_09
      && gtext_json_object_get(schema_doc, "$ref", 4) != NULL;
  for (size_t i = 0; i < obj_size; i++) {
    const char * key;
    size_t key_len;
    gtext_json_object_key(schema_doc, i, &key_len);
    key = gtext_json_object_key(schema_doc, i, NULL);
    const GTEXT_JSON_Value * value = gtext_json_object_value(schema_doc, i);

    /* A keyword from a vocabulary this dialect does not use is not a
     * keyword. It is an unknown member, and unknown members are ignored -
     * including by the check below that refuses the ones this engine cannot
     * enforce, because there is nothing to enforce. */
    if (!json_schema_keyword_in_use(cc->vocabularies, key, key_len)) {
      continue;
    }
    /* Nor is a keyword the draft in scope does not have yet. `prefixItems`
     * inside a 2019-09 document is not "items with a different name", it is
     * a member that draft never defined. */
    if (!json_schema_keyword_in_draft(cc->draft, key, key_len)) {
      continue;
    }
    /*
     * Before 2019-09, a schema object containing `$ref` is that reference and
     * nothing else: every other keyword beside it is ignored. 2019-09 made
     * `$ref` an applicator like any other, so its siblings apply. Reading a
     * draft-07 document under the newer rule adds constraints its author did
     * not write.
     */
    if (cc->draft < JSON_DRAFT_2019_09 && has_sibling_ref
        && !json_matches(key, key_len, "$ref")) {
      continue;
    }

    if (json_matches(key, key_len, "type")) {
      GTEXT_JSON_Status status = json_schema_parse_type(node, value, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "properties")) {
      if (value->type != GTEXT_JSON_OBJECT) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Properties must be an object"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      size_t prop_count = gtext_json_object_size(value);
      if (prop_count > 0) {
        // Allocate properties array
        if (node->properties_capacity < prop_count) {
          size_t new_capacity = prop_count;
          // Check for integer overflow in multiplication
          if (new_capacity > SIZE_MAX / sizeof(json_schema_property)) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Properties array size overflow"};
            }
            return GTEXT_JSON_E_OOM;
          }
          json_schema_property * new_props = (json_schema_property *)gtext_allocator_realloc(alloc, 
              node->properties, new_capacity * sizeof(json_schema_property));
          if (!new_props) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating properties"};
            }
            return GTEXT_JSON_E_OOM;
          }
          node->properties = new_props;
          node->properties_capacity = new_capacity;
        }

        // Compile each property schema
        for (size_t j = 0; j < prop_count; j++) {
          const char * prop_key;
          size_t prop_key_len;
          gtext_json_object_key(value, j, &prop_key_len);
          prop_key = gtext_json_object_key(value, j, NULL);
          const GTEXT_JSON_Value * prop_schema =
              gtext_json_object_value(value, j);

          // Allocate property entry
          json_schema_property * prop =
              &node->properties[node->properties_count];
          prop->key_len = prop_key_len;
          // Check for integer overflow in key_len + 1
          if (prop_key_len > SIZE_MAX - 1) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Property key length overflow"};
            }
            return GTEXT_JSON_E_OOM;
          }
          prop->key = (char *)gtext_allocator_malloc(alloc, prop_key_len + 1);
          if (!prop->key) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating property key"};
            }
            return GTEXT_JSON_E_OOM;
          }
          memcpy(prop->key, prop_key, prop_key_len);
          prop->key[prop_key_len] = '\0';

          // Compile property schema recursively
          prop->schema =
              (json_schema_node *)gtext_allocator_calloc(alloc, 1, sizeof(json_schema_node));
          if (!prop->schema) {
            gtext_allocator_free(alloc, prop->key);
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating property schema"};
            }
            return GTEXT_JSON_E_OOM;
          }

          GTEXT_JSON_Status status =
              json_schema_compile_node(prop->schema, prop_schema, cc, err);
          if (status != GTEXT_JSON_OK) {
            /* json_schema_node_free, not free: a node that failed part of
             * the way through still owns everything it compiled before the
             * keyword that failed, and it is not yet reachable from the
             * parent - properties_count has not been incremented - so
             * nothing else will ever free it. */
            gtext_allocator_free(alloc, prop->key);
            json_schema_node_free(prop->schema, alloc);
            prop->schema = NULL;
            return status;
          }

          node->properties_count++;
        }
      }
    }
    else if (json_matches(key, key_len, "required")) {
      if (value->type != GTEXT_JSON_ARRAY) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Required must be an array"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      size_t req_count = gtext_json_array_size(value);
      if (req_count > 0) {
        if (node->required_capacity < req_count) {
          size_t new_capacity = req_count;
          // Check for integer overflow in multiplication
          if (new_capacity > SIZE_MAX / sizeof(char *)) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Required keys array size overflow"};
            }
            return GTEXT_JSON_E_OOM;
          }
          char ** new_keys = (char **)gtext_allocator_realloc(alloc, 
              node->required_keys, new_capacity * sizeof(char *));
          if (!new_keys) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating required keys"};
            }
            return GTEXT_JSON_E_OOM;
          }
          node->required_keys = new_keys;
          node->required_capacity = new_capacity;
        }

        for (size_t j = 0; j < req_count; j++) {
          const GTEXT_JSON_Value * req_key = gtext_json_array_get(value, j);
          if (!req_key || req_key->type != GTEXT_JSON_STRING) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                  .message = "Required array must contain strings"};
            }
            return GTEXT_JSON_E_INVALID;
          }

          const char * req_key_str;
          size_t req_key_len;
          if (gtext_json_get_string(req_key, &req_key_str, &req_key_len) !=
              GTEXT_JSON_OK) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                  .message = "Invalid required key"};
            }
            return GTEXT_JSON_E_INVALID;
          }

          // Check for integer overflow in req_key_len + 1
          if (req_key_len > SIZE_MAX - 1) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Required key length overflow"};
            }
            return GTEXT_JSON_E_OOM;
          }
          node->required_keys[node->required_count] =
              (char *)gtext_allocator_malloc(alloc, req_key_len + 1);
          if (!node->required_keys[node->required_count]) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating required key"};
            }
            return GTEXT_JSON_E_OOM;
          }
          memcpy(node->required_keys[node->required_count], req_key_str,
              req_key_len);
          node->required_keys[node->required_count][req_key_len] = '\0';
          node->required_count++;
        }
      }
    }
    else if (json_matches(key, key_len, "items")) {
      /* draft-07 spells the positional form `items: [ ... ]`, which 2020-12
       * renamed to prefixItems.  Both compile to prefix_items, so a schema
       * written either way validates the same. */
      if (value->type == GTEXT_JSON_ARRAY) {
        GTEXT_JSON_Status status =
            json_schema_compile_sub_list(&node->prefix_items,
                &node->prefix_items_count, "items", value, cc, err);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
        continue;
      }
      // Compile items schema recursively
      node->items_schema =
          (json_schema_node *)gtext_allocator_calloc(alloc, 1, sizeof(json_schema_node));
      if (!node->items_schema) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
              .message = "Out of memory allocating items schema"};
        }
        return GTEXT_JSON_E_OOM;
      }

      GTEXT_JSON_Status status =
          json_schema_compile_node(node->items_schema, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        json_schema_node_free(node->items_schema, alloc);
        node->items_schema = NULL;
        return status;
      }
    }
    else if (json_matches(key, key_len, "enum")) {
      if (value->type != GTEXT_JSON_ARRAY) {
        if (err) {
          *err = (GTEXT_JSON_Error){
              .code = GTEXT_JSON_E_INVALID, .message = "Enum must be an array"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      /*
       * Recorded before the count is looked at: `"enum": []` is a valid
       * schema and no instance satisfies it, because the assertion is that
       * the instance equals one of the listed values and there are none.
       * Keying validation off enum_count alone made an empty enum compile
       * away to nothing and accept everything, which is the opposite answer.
       */
      node->has_enum = 1;
      size_t enum_count = gtext_json_array_size(value);
      if (enum_count > 0) {
        if (node->enum_capacity < enum_count) {
          size_t new_capacity = enum_count;
          // Check for integer overflow in multiplication
          if (new_capacity > SIZE_MAX / sizeof(GTEXT_JSON_Value *)) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Enum values array size overflow"};
            }
            return GTEXT_JSON_E_OOM;
          }
          GTEXT_JSON_Value ** new_enum = (GTEXT_JSON_Value **)gtext_allocator_realloc(alloc, 
              node->enum_values, new_capacity * sizeof(GTEXT_JSON_Value *));
          if (!new_enum) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating enum values"};
            }
            return GTEXT_JSON_E_OOM;
          }
          node->enum_values = new_enum;
          node->enum_capacity = new_capacity;
        }

        // Clone enum values into context
        // Initialize enum_count to 0 to ensure clean state
        node->enum_count = 0;
        for (size_t j = 0; j < enum_count; j++) {
          const GTEXT_JSON_Value * enum_val = gtext_json_array_get(value, j);
          if (!enum_val) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                  .message = "Invalid enum value"};
            }
            // Cleanup: enum_values already allocated, but enum_count is correct
            // Context will free any cloned values on error
            return GTEXT_JSON_E_INVALID;
          }

          // Clone value into context
          GTEXT_JSON_Value * cloned = json_value_clone(enum_val, cc->ctx);
          if (!cloned) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory cloning enum value"};
            }
            // Cleanup: enum_count is correct, context will free cloned values
            return GTEXT_JSON_E_OOM;
          }

          node->enum_values[node->enum_count++] = cloned;
        }
      }
    }
    else if (json_matches(key, key_len, "const")) {
      // Clone const value into context
      node->const_value = json_value_clone(value, cc->ctx);
      if (!node->const_value) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
              .message = "Out of memory cloning const value"};
        }
        return GTEXT_JSON_E_OOM;
      }
    }
    else if (json_matches(key, key_len, "minimum")) {
      if (value->type != GTEXT_JSON_NUMBER) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Minimum must be a number"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      double min_val;
      if (gtext_json_get_double(value, &min_val) != GTEXT_JSON_OK) {
        if (err) {
          *err = (GTEXT_JSON_Error){
              .code = GTEXT_JSON_E_INVALID, .message = "Invalid minimum value"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      node->has_minimum = 1;
      node->minimum = min_val;
    }
    else if (json_matches(key, key_len, "maximum")) {
      if (value->type != GTEXT_JSON_NUMBER) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Maximum must be a number"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      double max_val;
      if (gtext_json_get_double(value, &max_val) != GTEXT_JSON_OK) {
        if (err) {
          *err = (GTEXT_JSON_Error){
              .code = GTEXT_JSON_E_INVALID, .message = "Invalid maximum value"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      node->has_maximum = 1;
      node->maximum = max_val;
    }
    else if (json_matches(key, key_len, "minLength")) {
      if (value->type != GTEXT_JSON_NUMBER) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "MinLength must be a number"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      double min_len_val;
      if (gtext_json_get_double(value, &min_len_val) != GTEXT_JSON_OK ||
          min_len_val < 0) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Invalid minLength value"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      // Check for overflow when casting to size_t
      if (min_len_val > (double)SIZE_MAX) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "minLength value exceeds maximum size_t"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      node->has_min_length = 1;
      node->min_length = (size_t)min_len_val;
    }
    else if (json_matches(key, key_len, "maxLength")) {
      if (value->type != GTEXT_JSON_NUMBER) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "MaxLength must be a number"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      double max_len_val;
      if (gtext_json_get_double(value, &max_len_val) != GTEXT_JSON_OK ||
          max_len_val < 0) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Invalid maxLength value"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      // Check for overflow when casting to size_t
      if (max_len_val > (double)SIZE_MAX) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "maxLength value exceeds maximum size_t"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      node->has_max_length = 1;
      node->max_length = (size_t)max_len_val;
    }
    else if (json_matches(key, key_len, "minItems")) {
      if (value->type != GTEXT_JSON_NUMBER) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "MinItems must be a number"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      double min_items_val;
      if (gtext_json_get_double(value, &min_items_val) != GTEXT_JSON_OK ||
          min_items_val < 0) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Invalid minItems value"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      // Check for overflow when casting to size_t
      if (min_items_val > (double)SIZE_MAX) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "minItems value exceeds maximum size_t"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      node->has_min_items = 1;
      node->min_items = (size_t)min_items_val;
    }
    else if (json_matches(key, key_len, "maxItems")) {
      if (value->type != GTEXT_JSON_NUMBER) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "MaxItems must be a number"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      double max_items_val;
      if (gtext_json_get_double(value, &max_items_val) != GTEXT_JSON_OK ||
          max_items_val < 0) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Invalid maxItems value"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      // Check for overflow when casting to size_t
      if (max_items_val > (double)SIZE_MAX) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "maxItems value exceeds maximum size_t"};
        }
        return GTEXT_JSON_E_INVALID;
      }

      node->has_max_items = 1;
      node->max_items = (size_t)max_items_val;
    }
    // --- $ref ---------------------------------------------------------
    else if (json_matches(key, key_len, "$ref")) {
      if (value->type != GTEXT_JSON_STRING) {
        if (err) {
          *err = (GTEXT_JSON_Error){
              .code = GTEXT_JSON_E_INVALID, .message = "$ref must be a string"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      const char * rs = NULL;
      size_t rl = 0;
      if (gtext_json_get_string(value, &rs, &rl) != GTEXT_JSON_OK) {
        if (err) {
          *err = (GTEXT_JSON_Error){
              .code = GTEXT_JSON_E_INVALID, .message = "Invalid $ref value"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      cc->depth++;
      GTEXT_JSON_Status status =
          json_schema_resolve_ref(&node->ref_target, rs, rl, cc, err);
      cc->depth--;
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "$dynamicRef")) {
      /*
       * Resolved statically first, exactly as a `$ref` is - which is also
       * the answer for most of them. 2020-12 core section 8.2.3.2 makes the
       * reference dynamic only when its fragment is a plain name *and* the
       * schema it statically resolves to declares a `$dynamicAnchor` of that
       * name. A `$dynamicRef` to a pointer fragment, or to a plain anchor
       * that is not dynamic, is a `$ref` with a longer spelling.
       */
      const char * rs = NULL;
      size_t rl = 0;
      if (value->type != GTEXT_JSON_STRING
          || gtext_json_get_string(value, &rs, &rl) != GTEXT_JSON_OK) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "$dynamicRef must be a string"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      cc->depth++;
      GTEXT_JSON_Status status =
          json_schema_resolve_ref(&node->ref_target, rs, rl, cc, err);
      cc->depth--;
      if (status != GTEXT_JSON_OK) {
        return status;
      }
      const char * hash = (const char *)memchr(rs, '#', rl);
      if (hash) {
        const char * name = hash + 1;
        size_t name_len = rl - (size_t)(hash - rs) - 1;
        /*
         * The bookending requirement, asked of the target *schema* rather
         * than of the compiled node: does what this statically resolves to
         * declare a `$dynamicAnchor` of this name?
         *
         * Asked of the node it compiled to, it came out wrong. The registry
         * of compiled targets is keyed by URI, so the same schema reached as
         * ".../tree.json" and as ".../tree.json#node" compiles to two nodes,
         * and comparing pointers said the anchor did not match when it
         * plainly did. Every strict-tree-shaped schema then quietly became an
         * ordinary `$ref`.
         */
        int bookended = 0;
        if (name_len > 0 && name[0] != '/') {
          char * resolved = json_uri_resolve(
              cc->base_uri, strlen(cc->base_uri), rs, rl, alloc);
          if (resolved) {
            const GTEXT_JSON_Value * target =
                json_schema_find_target(cc, resolved, NULL, NULL);
            gtext_allocator_free(alloc, resolved);
            if (target && target->type == GTEXT_JSON_OBJECT) {
              const GTEXT_JSON_Value * declared =
                  gtext_json_object_get(target, "$dynamicAnchor", 14);
              bookended = declared && declared->type == GTEXT_JSON_STRING
                  && declared->as.string.len == name_len
                  && memcmp(declared->as.string.data, name, name_len) == 0;
            }
          }
        }
        if (bookended) {
          gtext_allocator_free(alloc, node->dynamic_ref_name);
          node->dynamic_ref_name = (char *)gtext_allocator_malloc(alloc, name_len + 1);
          if (!node->dynamic_ref_name) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory recording $dynamicRef"};
            }
            return GTEXT_JSON_E_OOM;
          }
          memcpy(node->dynamic_ref_name, name, name_len);
          node->dynamic_ref_name[name_len] = '\0';
          node->dynamic_ref_name_len = name_len;
        }
      }
    }
    else if (json_matches(key, key_len, "$recursiveRef")) {
      /*
       * 2019-09's dynamic reference, which 2020-12 replaced with
       * `$dynamicRef`. The rule is the same shape and the same scope walk
       * answers it, with one anonymous anchor name instead of many:
       *
       *   - it resolves statically first, exactly as `$ref` does;
       *   - it becomes dynamic only if what it resolves to declares
       *     `$recursiveAnchor: true`;
       *   - and then the target is the *outermost* resource on the dynamic
       *     path that also declares one.
       *
       * Where a resource does not declare the anchor, `$recursiveRef` is
       * `$ref` with a longer spelling - the same relationship `$dynamicRef`
       * has to a plain anchor.
       */
      const char * rs = NULL;
      size_t rl = 0;
      if (value->type != GTEXT_JSON_STRING
          || gtext_json_get_string(value, &rs, &rl) != GTEXT_JSON_OK) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "$recursiveRef must be a string"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      /*
       * 2019-09 core section 8.2.4.2.1 allows exactly one value. Accepting
       * anything else would mean guessing at a reference the specification
       * does not define, and guessing wrong is a wrong answer about the
       * instance rather than an unknown keyword.
       */
      if (rl != 1 || rs[0] != '#') {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA_UNSUPPORTED,
              .message = "$recursiveRef may only be \"#\""};
        }
        return GTEXT_JSON_E_SCHEMA_UNSUPPORTED;
      }
      cc->depth++;
      GTEXT_JSON_Status status =
          json_schema_resolve_ref(&node->ref_target, rs, rl, cc, err);
      cc->depth--;
      if (status != GTEXT_JSON_OK) {
        return status;
      }
      /*
       * The bookending check, asked of the target document the way the
       * `$dynamicRef` branch asks it - and for the same reason: the registry
       * of compiled nodes is keyed by URI, so one schema reached under two
       * URIs is two nodes and comparing pointers gives the wrong answer.
       */
      const GTEXT_JSON_Value * target =
          json_schema_find_target(cc, cc->base_uri, NULL, NULL);
      int bookended = 0;
      if (target && target->type == GTEXT_JSON_OBJECT) {
        const GTEXT_JSON_Value * declared =
            gtext_json_object_get(target, "$recursiveAnchor", 16);
        bool on = false;
        bookended = declared && declared->type == GTEXT_JSON_BOOL
            && gtext_json_get_bool(declared, &on) == GTEXT_JSON_OK && on;
      }
      if (bookended) {
        gtext_allocator_free(alloc, node->dynamic_ref_name);
        node->dynamic_ref_name = (char *)gtext_allocator_malloc(alloc, 1);
        if (!node->dynamic_ref_name) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory recording $recursiveRef"};
          }
          return GTEXT_JSON_E_OOM;
        }
        node->dynamic_ref_name[0] = '\0';
        node->dynamic_ref_name_len = 0;
      }
    }
    /* `$recursiveAnchor` is recorded before this loop runs, so there is
     * nothing to do for it here; it reaches this point only to be accepted
     * rather than fall through to the unsupported-keyword check. */
    else if (json_matches(key, key_len, "$recursiveAnchor")) {
      if (value->type != GTEXT_JSON_BOOL) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "$recursiveAnchor must be a boolean"};
        }
        return GTEXT_JSON_E_INVALID;
      }
    }
    // --- positional array schemas -------------------------------------
    else if (json_matches(key, key_len, "prefixItems")) {
      GTEXT_JSON_Status status =
          json_schema_compile_sub_list(&node->prefix_items,
              &node->prefix_items_count, "prefixItems", value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "additionalItems")) {
      GTEXT_JSON_Status status = json_schema_compile_sub(
          &node->additional_items, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    // --- contains -----------------------------------------------------
    else if (json_matches(key, key_len, "contains")) {
      GTEXT_JSON_Status status =
          json_schema_compile_sub(&node->contains_schema, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "minContains")
        || json_matches(key, key_len, "maxContains")) {
      double d = 0.0;
      if (value->type != GTEXT_JSON_NUMBER
          || gtext_json_get_double(value, &d) != GTEXT_JSON_OK || d < 0
          || d > (double)SIZE_MAX) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "minContains/maxContains must be a non-negative "
                         "integer"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      if (json_matches(key, key_len, "minContains")) {
        node->has_min_contains = 1;
        node->min_contains = (size_t)d;
      }
      else {
        node->has_max_contains = 1;
        node->max_contains = (size_t)d;
      }
    }
    // --- the two regular-expression keywords ---------------------------
    // Both are enforced when the caller supplied an engine and refused when
    // they did not.  The refusal is the same one any unenforceable keyword
    // gets, and for the same reason: a `pattern` that is read and ignored
    // makes a schema that looks like it constrains its data not do so, with
    // nothing to tell the caller.
    else if (json_matches(key, key_len, "unevaluatedItems")) {
      GTEXT_JSON_Status status =
          json_schema_compile_sub(&node->unevaluated_items, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "unevaluatedProperties")) {
      GTEXT_JSON_Status status = json_schema_compile_sub(
          &node->unevaluated_properties, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "format")) {
      /*
       * An annotation under the default policy, so nothing is kept and the
       * keyword constrains nothing - which is what 2020-12 requires.
       *
       * Under GTEXT_JSON_FORMAT_ASSERT a name the vocabulary defines but this
       * library cannot check is refused rather than ignored: the caller asked
       * for the constraint and would otherwise get a schema that does not
       * carry it, with no way to find out.  A name outside the vocabulary is
       * ignored, because the specification requires that.
       */
      /* Either the caller asked, or the dialect did: a metaschema that
       * declares the format-assertion vocabulary is saying that `format`
       * asserts in schemas written against it, which is the mechanism the
       * specification provides for exactly this. */
      if ((cc->opts->format == GTEXT_JSON_FORMAT_ASSERT
              || (cc->vocabularies & JSON_VOCAB_FORMAT_ASSERTION))
          && value->type == GTEXT_JSON_STRING) {
        const char * name = value->as.string.data;
        size_t name_len = value->as.string.len;
        if (json_format_is_known(name, name_len)) {
          /* `regex` is the caller's engine, so whether it can be checked
           * depends on whether one was supplied. Every other name in the
           * vocabulary is checked here. */
          int checkable = 1;
          if (name_len == 5 && memcmp(name, "regex", 5) == 0) {
            checkable = cc->schema->has_regex_provider;
          }
          if (!checkable) {
            if (!cc->opts->allow_unsupported_keywords) {
              return json_schema_reject_keyword(name, name_len, err);
            }
          }
          else {
            gtext_allocator_free(alloc, node->format_name);
            node->format_name = (char *)gtext_allocator_malloc(alloc, name_len + 1);
            if (!node->format_name) {
              if (err) {
                *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                    .message = "Out of memory recording format"};
              }
              return GTEXT_JSON_E_OOM;
            }
            memcpy(node->format_name, name, name_len);
            node->format_name[name_len] = '\0';
            node->format_name_len = name_len;
            node->regex_provider = cc->schema->has_regex_provider
                ? &cc->schema->regex_provider
                : NULL;
          }
        }
      }
    }
    else if (json_matches(key, key_len, "pattern")) {
      if (!cc->schema->has_regex_provider) {
        if (!cc->opts->allow_unsupported_keywords) {
          return json_schema_reject_keyword(key, key_len, err);
        }
      }
      else {
        GTEXT_JSON_Status status =
            json_schema_compile_regex(value, cc, &node->pattern_regex, err);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
        node->regex_provider = &cc->schema->regex_provider;
      }
    }
    else if (json_matches(key, key_len, "patternProperties")) {
      if (!cc->schema->has_regex_provider) {
        if (!cc->opts->allow_unsupported_keywords) {
          return json_schema_reject_keyword(key, key_len, err);
        }
      }
      else {
        if (value->type != GTEXT_JSON_OBJECT) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                .message = "patternProperties must be an object"};
          }
          return GTEXT_JSON_E_INVALID;
        }
        size_t n = gtext_json_object_size(value);
        if (n > 0) {
          if (n > SIZE_MAX / sizeof(json_schema_pattern_property)) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "patternProperties list too large"};
            }
            return GTEXT_JSON_E_OOM;
          }
          node->pattern_properties = (json_schema_pattern_property *)gtext_allocator_calloc(alloc, 
              n, sizeof(json_schema_pattern_property));
          if (!node->pattern_properties) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating patternProperties"};
            }
            return GTEXT_JSON_E_OOM;
          }
          /* The provider is recorded before the first pattern is compiled,
           * so that a failure part way through still frees the handles the
           * earlier iterations produced. */
          node->regex_provider = &cc->schema->regex_provider;
          for (size_t j = 0; j < n; j++) {
            size_t pat_len = 0;
            const char * pat = gtext_json_object_key(value, j, &pat_len);
            json_schema_pattern_property * entry =
                &node->pattern_properties[j];
            /* Counted before anything in the entry can fail, so that
             * json_schema_node_free() sees an entry it must clean up. */
            node->pattern_properties_count = j + 1;

            char message[JSON_SCHEMA_REGEX_MESSAGE_MAX];
            message[0] = '\0';
            size_t offset = (size_t)-1;
            const GTEXT_JSON_Regex_Provider * provider =
                &cc->schema->regex_provider;
            int rc = provider->compile_fn(provider->ctx, pat ? pat : "",
                pat_len, &entry->regex, message, sizeof(message), &offset);
            message[sizeof(message) - 1] = '\0';
            if (rc != 0 || !entry->regex) {
              entry->regex = NULL;
              return json_schema_reject_regex(
                  message[0] ? message
                             : "the regular-expression engine refused this "
                               "pattern",
                  offset, err);
            }

            const GTEXT_JSON_Value * sub = gtext_json_object_value(value, j);
            GTEXT_JSON_Status status =
                json_schema_compile_sub(&entry->schema, sub, cc, err);
            if (status != GTEXT_JSON_OK) {
              return status;
            }
          }
        }
      }
    }
    // --- object applicators -------------------------------------------
    else if (json_matches(key, key_len, "additionalProperties")) {
      GTEXT_JSON_Status status = json_schema_compile_sub(
          &node->additional_properties, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "propertyNames")) {
      GTEXT_JSON_Status status =
          json_schema_compile_sub(&node->property_names, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "dependencies")) {
      /* draft-07's `dependencies` is the union of what 2020-12 split into
       * dependentRequired and dependentSchemas: each value is either an array
       * of property names or a schema.  Rather than a third structure, each
       * entry is compiled into whichever of the two it means. */
      if (value->type != GTEXT_JSON_OBJECT) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "dependencies must be an object"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      size_t n = gtext_json_object_size(value);
      for (size_t i = 0; i < n; i++) {
        size_t dk_len = 0;
        const char * dk = gtext_json_object_key(value, i, &dk_len);
        const GTEXT_JSON_Value * dv = gtext_json_object_value(value, i);
        if (!dv) {
          continue;
        }
        if (dv->type == GTEXT_JSON_ARRAY) {
          size_t rn = gtext_json_array_size(dv);
          json_schema_dep_required * grown =
              (json_schema_dep_required *)gtext_allocator_realloc(alloc, node->dep_required,
                  (node->dep_required_count + 1)
                      * sizeof(json_schema_dep_required));
          if (!grown) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating dependencies"};
            }
            return GTEXT_JSON_E_OOM;
          }
          node->dep_required = grown;
          json_schema_dep_required * entry =
              &node->dep_required[node->dep_required_count];
          memset(entry, 0, sizeof(*entry));
          entry->key = (char *)gtext_allocator_malloc(alloc, dk_len + 1);
          if (!entry->key) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating dependencies key"};
            }
            return GTEXT_JSON_E_OOM;
          }
          memcpy(entry->key, dk, dk_len);
          entry->key[dk_len] = '\0';
          node->dep_required_count++;
          if (rn > 0) {
            entry->required = (char **)gtext_allocator_calloc(alloc, rn, sizeof(char *));
            if (!entry->required) {
              if (err) {
                *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                    .message = "Out of memory allocating dependencies list"};
              }
              return GTEXT_JSON_E_OOM;
            }
          }
          for (size_t j = 0; j < rn; j++) {
            const GTEXT_JSON_Value * name = gtext_json_array_get(dv, j);
            const char * ns = NULL;
            size_t nl = 0;
            if (!name || name->type != GTEXT_JSON_STRING
                || gtext_json_get_string(name, &ns, &nl) != GTEXT_JSON_OK) {
              if (err) {
                *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                    .message = "dependencies names must be strings"};
              }
              return GTEXT_JSON_E_INVALID;
            }
            entry->required[j] = (char *)gtext_allocator_malloc(alloc, nl + 1);
            if (!entry->required[j]) {
              if (err) {
                *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                    .message = "Out of memory allocating dependencies name"};
              }
              return GTEXT_JSON_E_OOM;
            }
            memcpy(entry->required[j], ns ? ns : "", nl);
            entry->required[j][nl] = '\0';
            entry->required_count = j + 1;
          }
        }
        else {
          json_schema_dep_schema * grown =
              (json_schema_dep_schema *)gtext_allocator_realloc(alloc, node->dep_schemas,
                  (node->dep_schemas_count + 1)
                      * sizeof(json_schema_dep_schema));
          if (!grown) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating dependencies"};
            }
            return GTEXT_JSON_E_OOM;
          }
          node->dep_schemas = grown;
          json_schema_dep_schema * entry =
              &node->dep_schemas[node->dep_schemas_count];
          memset(entry, 0, sizeof(*entry));
          entry->key = (char *)gtext_allocator_malloc(alloc, dk_len + 1);
          if (!entry->key) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating dependencies key"};
            }
            return GTEXT_JSON_E_OOM;
          }
          memcpy(entry->key, dk, dk_len);
          entry->key[dk_len] = '\0';
          node->dep_schemas_count++;
          GTEXT_JSON_Status status =
              json_schema_compile_sub(&entry->schema, dv, cc, err);
          if (status != GTEXT_JSON_OK) {
            return status;
          }
        }
      }
    }
    else if (json_matches(key, key_len, "dependentSchemas")) {
      if (value->type != GTEXT_JSON_OBJECT) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "dependentSchemas must be an object"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      size_t n = gtext_json_object_size(value);
      if (n > 0) {
        node->dep_schemas = (json_schema_dep_schema *)gtext_allocator_calloc(alloc, 
            n, sizeof(json_schema_dep_schema));
        if (!node->dep_schemas) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory allocating dependentSchemas"};
          }
          return GTEXT_JSON_E_OOM;
        }
      }
      for (size_t i = 0; i < n; i++) {
        size_t dk_len = 0;
        const char * dk = gtext_json_object_key(value, i, &dk_len);
        const GTEXT_JSON_Value * sub = gtext_json_object_value(value, i);
        json_schema_dep_schema * entry = &node->dep_schemas[i];
        entry->key = (char *)gtext_allocator_malloc(alloc, dk_len + 1);
        if (!entry->key) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory allocating dependentSchemas key"};
          }
          return GTEXT_JSON_E_OOM;
        }
        memcpy(entry->key, dk, dk_len);
        entry->key[dk_len] = '\0';
        node->dep_schemas_count = i + 1;
        GTEXT_JSON_Status status =
            json_schema_compile_sub(&entry->schema, sub, cc, err);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
      }
    }
    // --- numeric assertions -------------------------------------------
    /*
     * draft-04 spells these two as booleans that modify `minimum` and
     * `maximum` rather than as bounds of their own: `{"minimum": 3,
     * "exclusiveMinimum": true}` is `> 3`. Recorded here and applied after the
     * loop, because the keywords arrive in whatever order the document wrote
     * them and the bound this one modifies may not have been read yet.
     */
    else if (cc->draft <= JSON_DRAFT_04
        && (json_matches(key, key_len, "exclusiveMinimum")
            || json_matches(key, key_len, "exclusiveMaximum"))) {
      if (value->type != GTEXT_JSON_BOOL) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "In draft-04 exclusiveMinimum and exclusiveMaximum "
                         "are booleans that modify minimum and maximum"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      bool on = false;
      if (gtext_json_get_bool(value, &on) != GTEXT_JSON_OK) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Invalid boolean exclusive bound"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      if (json_matches(key, key_len, "exclusiveMinimum")) {
        draft4_exclusive_minimum = on ? 1 : 0;
      }
      else {
        draft4_exclusive_maximum = on ? 1 : 0;
      }
    }
    else if (json_matches(key, key_len, "exclusiveMinimum")
        || json_matches(key, key_len, "exclusiveMaximum")
        || json_matches(key, key_len, "multipleOf")) {
      if (value->type != GTEXT_JSON_NUMBER) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Numeric keyword requires a number"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      double d = 0.0;
      if (gtext_json_get_double(value, &d) != GTEXT_JSON_OK) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Numeric keyword value is not representable"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      if (json_matches(key, key_len, "exclusiveMinimum")) {
        node->has_exclusive_minimum = 1;
        node->exclusive_minimum = d;
      }
      else if (json_matches(key, key_len, "exclusiveMaximum")) {
        node->has_exclusive_maximum = 1;
        node->exclusive_maximum = d;
      }
      else {
        /* multipleOf must be strictly greater than zero; zero would make
         * every instance fail a division that has no meaning. */
        if (!(d > 0.0)) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                .message = "multipleOf must be greater than zero"};
          }
          return GTEXT_JSON_E_INVALID;
        }
        node->has_multiple_of = 1;
        node->multiple_of = d;
        node->has_multiple_of_decimal =
            (value->type == GTEXT_JSON_NUMBER && value->as.number.lexeme
                && json_decimal_from_lexeme(value->as.number.lexeme,
                    value->as.number.lexeme_len,
                    &node->multiple_of_decimal));
      }
    }
    // --- object and array size assertions -----------------------------
    else if (json_matches(key, key_len, "minProperties")
        || json_matches(key, key_len, "maxProperties")) {
      double d = 0.0;
      if (value->type != GTEXT_JSON_NUMBER
          || gtext_json_get_double(value, &d) != GTEXT_JSON_OK || d < 0
          || d > (double)SIZE_MAX) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "minProperties/maxProperties must be a "
                         "non-negative integer"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      if (json_matches(key, key_len, "minProperties")) {
        node->has_min_properties = 1;
        node->min_properties = (size_t)d;
      }
      else {
        node->has_max_properties = 1;
        node->max_properties = (size_t)d;
      }
    }
    else if (json_matches(key, key_len, "uniqueItems")) {
      if (value->type != GTEXT_JSON_BOOL) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "uniqueItems must be a boolean"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      bool b = false;
      if (gtext_json_get_bool(value, &b) != GTEXT_JSON_OK) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "uniqueItems must be a boolean"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      node->has_unique_items = 1;
      node->unique_items = b ? 1 : 0;
    }
    // --- boolean applicators ------------------------------------------
    else if (json_matches(key, key_len, "allOf")) {
      GTEXT_JSON_Status status = json_schema_compile_sub_list(
          &node->all_of, &node->all_of_count, "allOf", value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "anyOf")) {
      GTEXT_JSON_Status status = json_schema_compile_sub_list(
          &node->any_of, &node->any_of_count, "anyOf", value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "oneOf")) {
      GTEXT_JSON_Status status = json_schema_compile_sub_list(
          &node->one_of, &node->one_of_count, "oneOf", value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "not")) {
      GTEXT_JSON_Status status =
          json_schema_compile_sub(&node->not_schema, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "if")) {
      GTEXT_JSON_Status status =
          json_schema_compile_sub(&node->if_schema, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "then")) {
      GTEXT_JSON_Status status =
          json_schema_compile_sub(&node->then_schema, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    else if (json_matches(key, key_len, "else")) {
      GTEXT_JSON_Status status =
          json_schema_compile_sub(&node->else_schema, value, cc, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
    // --- dependentRequired --------------------------------------------
    else if (json_matches(key, key_len, "dependentRequired")) {
      if (value->type != GTEXT_JSON_OBJECT) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "dependentRequired must be an object"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      size_t n = gtext_json_object_size(value);
      if (n > 0) {
        node->dep_required = (json_schema_dep_required *)gtext_allocator_calloc(alloc, 
            n, sizeof(json_schema_dep_required));
        if (!node->dep_required) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory allocating dependentRequired"};
          }
          return GTEXT_JSON_E_OOM;
        }
      }
      for (size_t i = 0; i < n; i++) {
        size_t dk_len = 0;
        const char * dk = gtext_json_object_key(value, i, &dk_len);
        const GTEXT_JSON_Value * list = gtext_json_object_value(value, i);
        if (!list || list->type != GTEXT_JSON_ARRAY) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                .message = "dependentRequired values must be arrays of names"};
          }
          return GTEXT_JSON_E_INVALID;
        }
        json_schema_dep_required * entry = &node->dep_required[i];
        entry->key = (char *)gtext_allocator_malloc(alloc, dk_len + 1);
        if (!entry->key) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory allocating dependentRequired key"};
          }
          return GTEXT_JSON_E_OOM;
        }
        memcpy(entry->key, dk, dk_len);
        entry->key[dk_len] = '\0';
        node->dep_required_count = i + 1;

        size_t rn = gtext_json_array_size(list);
        if (rn > 0) {
          entry->required = (char **)gtext_allocator_calloc(alloc, rn, sizeof(char *));
          if (!entry->required) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating dependentRequired list"};
            }
            return GTEXT_JSON_E_OOM;
          }
        }
        for (size_t j = 0; j < rn; j++) {
          const GTEXT_JSON_Value * name = gtext_json_array_get(list, j);
          if (!name || name->type != GTEXT_JSON_STRING) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                  .message = "dependentRequired names must be strings"};
            }
            return GTEXT_JSON_E_INVALID;
          }
          size_t nl = 0;
          const char * ns = NULL;
          if (gtext_json_get_string(name, &ns, &nl) != GTEXT_JSON_OK) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
                  .message = "dependentRequired names must be strings"};
            }
            return GTEXT_JSON_E_INVALID;
          }
          entry->required[j] = (char *)gtext_allocator_malloc(alloc, nl + 1);
          if (!entry->required[j]) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                  .message = "Out of memory allocating dependentRequired name"};
            }
            return GTEXT_JSON_E_OOM;
          }
          memcpy(entry->required[j], ns ? ns : "", nl);
          entry->required[j][nl] = '\0';
          entry->required_count = j + 1;
        }
      }
    }
    else if (!cc->opts->allow_unsupported_keywords &&
             json_schema_keyword_in(
                 json_schema_unsupported_keywords, key, key_len)) {
      return json_schema_reject_keyword(key, key_len, err);
    }
    // Everything else is ignored, because none of it can change which
    // instances are valid.  That covers the annotation keywords (title,
    // description, default, examples, $comment, readOnly, writeOnly,
    // deprecated); the core plumbing that is inert while $ref is
    // unsupported ($schema selects a dialect when only one is implemented,
    // $defs and definitions are containers nothing can reach, $id and
    // $anchor name a base URI nothing resolves against, $vocabulary); and
    // vendor extensions and keywords from newer drafts, which JSON Schema
    // requires an implementation to ignore.
  }

  /*
   * `items` names the tail when `prefixItems` is present.
   *
   * 2020-12 core section 10.3.1.2 is explicit: `items` applies to every
   * element "at an index greater than the length of prefixItems".  This
   * compiled object-valued `items` to items_schema unconditionally, which is
   * draft-07's rule, and so `{"prefixItems": [{}, {}, {}], "items": false}`
   * called every array invalid - including the empty one.  That is not a
   * near-miss: it rejects documents the specification calls valid, and the
   * header described the equivalence as deliberate.
   *
   * The decision is made here rather than in the `items` branch because
   * object keys arrive in document order, and a schema is free to write
   * `items` before `prefixItems`.
   *
   * The test is prefix_items_count rather than "prefixItems was present", and
   * the two differ only for `"prefixItems": []`, which covers no element - so
   * `items` starts at index 0 either way and the readings agree.
   */
  /*
   * `additionalItems` governs the tail of a *positional* `items` and nothing
   * else.  2019-09 core section 9.3.1.2, and draft-07 and draft-06 before it,
   * say that if `items` is absent or is a single schema then `additionalItems`
   * is ignored entirely - the single schema already covers every element, so
   * there is no tail for a second keyword to describe.
   *
   * This engine applied it to every element from prefix_items_count onward,
   * which is zero when there is no positional list, so
   * `{"items": {"type": "integer"}, "additionalItems": false}` called every
   * non-empty array invalid and `{"additionalItems": false}` on its own called
   * every array invalid.  Both are valid-for-anything schemas.
   *
   * 2020-12 removed the keyword, so there it is an unknown member and never
   * reaches here.
   */
  /*
   * draft-04's exclusive bounds, now that both halves have been read.
   *
   * `true` turns the bound it names into an exclusive one; `false` is the
   * default spelled out and changes nothing. Either without the bound it
   * modifies is refused: draft-04's meta-schema makes each depend on the other,
   * and a lone `"exclusiveMinimum": true` asserts a bound whose value is
   * nowhere in the document.
   */
  if (draft4_exclusive_minimum >= 0) {
    if (!node->has_minimum) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "draft-04 exclusiveMinimum needs a minimum to modify"};
      }
      return GTEXT_JSON_E_INVALID;
    }
    if (draft4_exclusive_minimum == 1) {
      node->has_exclusive_minimum = 1;
      node->exclusive_minimum = node->minimum;
      node->has_minimum = 0;
    }
  }
  if (draft4_exclusive_maximum >= 0) {
    if (!node->has_maximum) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "draft-04 exclusiveMaximum needs a maximum to modify"};
      }
      return GTEXT_JSON_E_INVALID;
    }
    if (draft4_exclusive_maximum == 1) {
      node->has_exclusive_maximum = 1;
      node->exclusive_maximum = node->maximum;
      node->has_maximum = 0;
    }
  }

  if (cc->draft < JSON_DRAFT_2020_12 && node->additional_items) {
    const GTEXT_JSON_Value * items =
        gtext_json_object_get(schema_doc, "items", 5);
    if (!items || items->type != GTEXT_JSON_ARRAY) {
      json_schema_node_free(node->additional_items, alloc);
      node->additional_items = NULL;
    }
  }

  if (node->prefix_items_count > 0 && node->items_schema) {
    if (node->additional_items) {
      /*
       * `additionalItems` is draft-07's name for this slot and `items` is
       * 2020-12's, so a schema carrying both has named it twice, and the two
       * drafts disagree about which one to honor: 2020-12 ignores
       * `additionalItems` as an unknown keyword, draft-07 ignores
       * `prefixItems` and lets `items` govern every element.  Nothing in the
       * document says which draft it was written for, and picking one of two
       * meanings in silence is the failure this engine exists not to have.
       */
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Schema uses both \"items\" after \"prefixItems\" and "
                       "\"additionalItems\" for the same elements; these are "
                       "two drafts' names for one keyword and they do not "
                       "agree here"};
      }
      return GTEXT_JSON_E_INVALID;
    }
    node->additional_items = node->items_schema;
    node->items_schema = NULL;
  }

  return GTEXT_JSON_OK;
}


/*
 * Does `instance` satisfy this node's `multipleOf`?
 *
 * Asked in decimal whenever both sides kept the digits they were written
 * with, because that is the question JSON Schema validation section 6.2.1
 * asks: `0.0075` is 75 lots of `0.0001` in the decimal the document wrote,
 * and is not a whole number of them in the binary either value rounds to.
 * This used fmod on doubles and so called the spec's own example invalid.
 *
 * The binary path remains for the cases the decimal one cannot serve: a
 * lexeme longer than a uint64 mantissa holds, and a value built through the
 * DOM API or parsed with preserve_number_lexeme turned off, neither of which
 * has digits to read.  It is a fallback rather than a second policy - it
 * answers the same question less exactly - and `has_multiple_of_decimal`
 * says which one answered.
 */
/*
 * Is this instance a whole number?
 *
 * The digits the document wrote decide it, and the double only when there are
 * no digits to consult - a value built through the DOM API rather than parsed.
 *
 * This used to be `dv == (double)(long long)dv`, which is wrong twice over.
 * Converting a double outside `long long`'s range is undefined behaviour, so
 * the answer for anything past about 9.2e18 was whatever the hardware felt
 * like; and on x86-64 what it feels like is `LLONG_MIN`, which compares unequal
 * and so reported every large integer as not an integer. A fifty-three digit
 * whole number is an integer, and so is 1e308.
 */
static int json_schema_is_integral(const GTEXT_JSON_Value * instance) {
  if (instance->as.number.lexeme) {
    int whole = json_decimal_lexeme_is_integer(
        instance->as.number.lexeme, instance->as.number.lexeme_len);
    if (whole >= 0) {
      return whole;
    }
  }
  double dv = 0.0;
  if (gtext_json_get_double(instance, &dv) != GTEXT_JSON_OK) {
    return 0;
  }
  /* `floor` rather than a cast: it is defined for every finite double, and
   * every double of magnitude 2^52 or more is already whole. */
  return isfinite(dv) && dv == floor(dv);
}

/*
 * Is this number *written* as an integer, which is draft-04's question?
 *
 * draft-04 section 3.5 defines the primitive types over the JSON text, so a
 * fraction or an exponent makes a number a "number" and not an "integer" even
 * when its value is whole: `1.0` and `1e2` are not integers there, and are in
 * every draft from draft-06 on. The lexeme is the only place that distinction
 * survives - by the time it is a double, 1 and 1.0 are one value - so a number
 * that arrived without one cannot be asked, and falls back to the value.
 */
static int json_schema_is_written_as_integer(const GTEXT_JSON_Value * instance) {
  const char * lexeme = instance->as.number.lexeme;
  if (!lexeme) {
    return json_schema_is_integral(instance);
  }
  size_t len = instance->as.number.lexeme_len;
  for (size_t i = 0; i < len; i++) {
    if (lexeme[i] == '.' || lexeme[i] == 'e' || lexeme[i] == 'E') {
      return 0;
    }
  }
  return 1;
}

static int json_schema_is_multiple_of(
    const json_schema_node * node, const GTEXT_JSON_Value * instance, double v) {
  if (node->has_multiple_of_decimal && instance->type == GTEXT_JSON_NUMBER
      && instance->as.number.lexeme) {
    json_decimal value;
    if (json_decimal_from_lexeme(instance->as.number.lexeme,
            instance->as.number.lexeme_len, &value)) {
      int exact = json_decimal_is_multiple_of(&value, &node->multiple_of_decimal);
      if (exact >= 0) {
        return exact;
      }
    }
  }
  return fmod(v, node->multiple_of) == 0.0;
}

/*
 * Run one compiled pattern over one string.
 *
 * Three outcomes, not two.  A provider that could not finish - a step budget
 * spent on a pattern whose worst case is exponential, an allocation refused -
 * has not said the instance is invalid, and recording that as "no match" would
 * turn a denial-of-service defence into a wrong answer.  It becomes
 * GTEXT_JSON_E_LIMIT, which is neither OK nor E_SCHEMA, and the caller can
 * tell the two apart.
 */
static GTEXT_JSON_Status json_schema_regex_search(
    const json_schema_node * node, void * regex, const char * subject,
    size_t subject_len, int * out_matched, GTEXT_JSON_Error * err) {
  const GTEXT_JSON_Regex_Provider * provider = node->regex_provider;
  int rc = provider->search_fn(
      provider->ctx, regex, subject ? subject : "", subject_len);
  if (rc < 0) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_LIMIT,
          .message = "The regular-expression engine could not finish this "
                     "search; the instance is neither valid nor invalid"};
    }
    return GTEXT_JSON_E_LIMIT;
  }
  *out_matched = rc > 0;
  return GTEXT_JSON_OK;
}

/*
 * The length of a string instance, in characters.
 *
 * JSON Schema validation section 6.3 defines `minLength` and `maxLength` over
 * "the number of its characters as defined by RFC 8259", and an RFC 8259
 * string is a sequence of Unicode code points.  This counted bytes, so every
 * non-ASCII instance was measured wrong: `{"maxLength": 1}` called "e-acute"
 * too long, and `{"minLength": 2}` called it long enough.  Neither is a
 * borderline reading of the specification.
 *
 * Code points, not UTF-16 code units: an astral character such as U+1F4A9 is
 * one character here even though ECMAScript's own `.length` says two.  The
 * published test suite has that case precisely because it is the one an
 * implementation is most likely to get wrong.
 *
 * Counting the bytes that are not continuation bytes is exact for
 * well-formed UTF-8 and cannot run past the end of the buffer for anything
 * else - which matters, because the parser only validates UTF-8 when it is
 * asked to, so a caller who turned that off can reach here with bytes that
 * decode to nothing.  An approximate count on input that is already invalid
 * is the right failure; walking off the end is not.
 */
static size_t json_schema_string_length(const char * bytes, size_t byte_len) {
  size_t characters = 0;
  for (size_t i = 0; i < byte_len; i++) {
    if (((unsigned char)bytes[i] & 0xC0) != 0x80) {
      characters++;
    }
  }
  return characters;
}

/*
 * The chain of schema resources validation has passed through.
 *
 * `$dynamicRef` is the one keyword whose target is not known until validation
 * runs: it looks for the *outermost* resource in this chain that declares a
 * `$dynamicAnchor` of the name it wants. That is what lets a schema extend
 * another one and have the other one's internal references come back to the
 * extension - the recursive-tree pattern the specification is built around.
 *
 * A link per frame on the C stack, so nothing is allocated and the chain
 * unwinds itself. Innermost first, which is why the search below keeps the
 * last match rather than the first.
 */
typedef struct json_schema_scope {
  const struct json_schema_scope * parent;
  size_t resource_slot;
} json_schema_scope;

/*
 * What a schema object has already reached, for the two keywords that ask.
 *
 * `unevaluatedItems` and `unevaluatedProperties` apply to whatever nothing
 * else in scope applied to, so validation has to carry that around. One byte
 * per element of an array instance or per property of an object one, which
 * makes "was this reached" a lookup rather than a search and costs nothing
 * for the instances that have no unevaluated keyword above them - no
 * annotations are collected at all unless some node asks for them.
 *
 * The rule that makes this more than bookkeeping: a subschema that *failed*
 * contributes nothing. An `anyOf` whose first branch matched half the
 * properties and then failed has not evaluated them, and merging only on
 * success is what keeps that straight.
 */
typedef struct {
  unsigned char * marks;
  size_t count;
  /**
   * Where `marks` comes from, recorded by json_schema_eval_init() before it
   * allocates and never reassigned, so init and clear cannot name different
   * allocators for one buffer - the csv_field_buffer_init rule.
   *
   * Validation is reached with a node and not with the schema, so the
   * allocator arrives from json_schema_node::owner, which
   * json_schema_compile_node() sets on every node it compiles.
   */
  const GTEXT_Allocator * alloc;
} json_schema_eval;

static GTEXT_JSON_Status json_schema_eval_init(json_schema_eval * eval,
    const GTEXT_JSON_Value * instance, const GTEXT_Allocator * alloc) {
  eval->marks = NULL;
  eval->count = 0;
  eval->alloc = alloc;
  if (!instance) {
    return GTEXT_JSON_OK;
  }
  if (instance->type == GTEXT_JSON_ARRAY) {
    eval->count = gtext_json_array_size(instance);
  }
  else if (instance->type == GTEXT_JSON_OBJECT) {
    eval->count = gtext_json_object_size(instance);
  }
  if (eval->count == 0) {
    return GTEXT_JSON_OK;
  }
  eval->marks =
      (unsigned char *)gtext_allocator_calloc(eval->alloc, eval->count, 1);
  return eval->marks ? GTEXT_JSON_OK : GTEXT_JSON_E_OOM;
}

static void json_schema_eval_clear(json_schema_eval * eval) {
  gtext_allocator_free(eval->alloc, eval->marks);
  eval->marks = NULL;
  eval->count = 0;
}

static void json_schema_eval_mark(json_schema_eval * eval, size_t index) {
  if (eval && eval->marks && index < eval->count) {
    eval->marks[index] = 1;
  }
}

static void json_schema_eval_merge(
    json_schema_eval * into, const json_schema_eval * from) {
  if (!into || !into->marks || !from->marks) {
    return;
  }
  size_t n = into->count < from->count ? into->count : from->count;
  for (size_t i = 0; i < n; i++) {
    into->marks[i] |= from->marks[i];
  }
}

static GTEXT_JSON_Status json_schema_validate_body(
    const json_schema_node * node, const GTEXT_JSON_Value * instance,
    int depth, json_schema_eval * eval,
    const json_schema_scope * scope, GTEXT_JSON_Error * err);

static GTEXT_JSON_Status json_schema_validate_depth(
    const json_schema_node * node, const GTEXT_JSON_Value * instance,
    int depth, json_schema_eval * eval,
    const json_schema_scope * scope, GTEXT_JSON_Error * err);

/*
 * Kept so the existing call sites read the same.  Every recursive call goes
 * through the depth-carrying form: a schema like {"$ref":"#"} consumes no
 * instance as it recurses, so instance depth is not a bound.
 */
static GTEXT_JSON_Status json_schema_validate_node(
    const json_schema_node * node, const GTEXT_JSON_Value * instance,
    GTEXT_JSON_Error * err) {
  return json_schema_validate_depth(node, instance, 0, NULL, NULL, err);
}

/*
 * Apply `unevaluatedItems` and `unevaluatedProperties` to what is left.
 *
 * Runs after everything else in the schema object, which is not an
 * optimisation but the definition: the set these two apply to is "whatever
 * the rest of this schema did not reach", and it is not known until the rest
 * has run. What they do reach becomes evaluated in turn, so a parent that
 * asks the same question of the same instance sees them as covered.
 */
static GTEXT_JSON_Status json_schema_apply_unevaluated(
    const json_schema_node * node, const GTEXT_JSON_Value * instance,
    int depth, json_schema_eval * eval,
    const json_schema_scope * scope, GTEXT_JSON_Error * err) {
  if (node->unevaluated_items && instance->type == GTEXT_JSON_ARRAY) {
    size_t count = gtext_json_array_size(instance);
    for (size_t i = 0; i < count; i++) {
      if (eval->marks && i < eval->count && eval->marks[i]) {
        continue;
      }
      const GTEXT_JSON_Value * item = gtext_json_array_get(instance, i);
      if (!item) {
        continue;
      }
      GTEXT_JSON_Status status =
          json_schema_validate_depth(
              node->unevaluated_items, item, depth + 1, NULL, scope, err);
      if (status != GTEXT_JSON_OK) {
        if (err && err->code == GTEXT_JSON_E_SCHEMA) {
          err->message = "An item is not allowed by unevaluatedItems";
        }
        return status;
      }
      json_schema_eval_mark(eval, i);
    }
  }
  if (node->unevaluated_properties && instance->type == GTEXT_JSON_OBJECT) {
    size_t count = gtext_json_object_size(instance);
    for (size_t i = 0; i < count; i++) {
      if (eval->marks && i < eval->count && eval->marks[i]) {
        continue;
      }
      const GTEXT_JSON_Value * value = gtext_json_object_value(instance, i);
      if (!value) {
        continue;
      }
      GTEXT_JSON_Status status = json_schema_validate_depth(
          node->unevaluated_properties, value, depth + 1, NULL, scope, err);
      if (status != GTEXT_JSON_OK) {
        if (err && err->code == GTEXT_JSON_E_SCHEMA) {
          err->message = "A property is not allowed by unevaluatedProperties";
        }
        return status;
      }
      json_schema_eval_mark(eval, i);
    }
  }
  return GTEXT_JSON_OK;
}

/*
 * Validate one schema against one instance.
 *
 * A node with neither unevaluated keyword writes its annotations straight
 * into whatever its caller passed, which is how an `allOf` branch's
 * `properties` becomes visible to an `unevaluatedProperties` several levels
 * above it. A node that has one keeps its own set instead, because those two
 * see what their *own* schema object reached and not what a sibling did.
 */
static GTEXT_JSON_Status json_schema_validate_depth(
    const json_schema_node * node, const GTEXT_JSON_Value * instance,
    int depth, json_schema_eval * eval,
    const json_schema_scope * outer, GTEXT_JSON_Error * err) {
  /*
   * Entering a schema resource extends the dynamic scope. Pushed only when
   * the resource actually changes, so the chain is the sequence of distinct
   * resources on the path rather than one link per subschema - which is what
   * `$dynamicRef` is defined over.
   */
  json_schema_scope pushed;
  const json_schema_scope * scope = outer;
  if (node && node->resource_slot != 0
      && (!outer || outer->resource_slot != node->resource_slot)) {
    pushed.parent = outer;
    pushed.resource_slot = node->resource_slot;
    scope = &pushed;
  }

  if (!node || !instance
      || (!node->unevaluated_items && !node->unevaluated_properties)) {
    return json_schema_validate_body(node, instance, depth, eval, scope, err);
  }

  json_schema_eval local;
  if (json_schema_eval_init(&local, instance,
          node->owner && node->owner->ctx ? node->owner->ctx->alloc : NULL)
      != GTEXT_JSON_OK) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory tracking what a schema evaluated"};
    }
    return GTEXT_JSON_E_OOM;
  }
  GTEXT_JSON_Status status =
      json_schema_validate_body(node, instance, depth, &local, scope, err);
  if (status == GTEXT_JSON_OK) {
    status = json_schema_apply_unevaluated(node, instance, depth, &local, scope, err);
  }
  if (status == GTEXT_JSON_OK) {
    json_schema_eval_merge(eval, &local);
  }
  json_schema_eval_clear(&local);
  return status;
}

/*
 * One in-place applicator: the same instance, a different schema.
 *
 * Its annotations join the caller's only if it passed, which is the rule that
 * makes `anyOf` and `oneOf` come out right.
 */
static GTEXT_JSON_Status json_schema_validate_inplace(
    const json_schema_node * node, const GTEXT_JSON_Value * instance,
    int depth, json_schema_eval * eval,
    const json_schema_scope * scope, GTEXT_JSON_Error * err) {
  if (!eval || !eval->marks) {
    return json_schema_validate_depth(node, instance, depth, NULL, scope, err);
  }
  json_schema_eval sub;
  if (json_schema_eval_init(&sub, instance, eval->alloc) != GTEXT_JSON_OK) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory tracking what a schema evaluated"};
    }
    return GTEXT_JSON_E_OOM;
  }
  GTEXT_JSON_Status status =
      json_schema_validate_depth(node, instance, depth, &sub, scope, err);
  if (status == GTEXT_JSON_OK) {
    json_schema_eval_merge(eval, &sub);
  }
  json_schema_eval_clear(&sub);
  return status;
}

static GTEXT_JSON_Status json_schema_validate_body(
    const json_schema_node * node, const GTEXT_JSON_Value * instance,
    int depth, json_schema_eval * eval,
    const json_schema_scope * scope, GTEXT_JSON_Error * err) {
  if (depth >= JSON_SCHEMA_MAX_VALIDATE_DEPTH) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_DEPTH,
          .message = "Schema validation nested too deeply"};
    }
    return GTEXT_JSON_E_DEPTH;
  }

  if (!node || !instance) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments to schema validation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // A boolean schema settles it outright.
  if (node->is_bool_schema) {
    if (node->bool_schema_value) {
      return GTEXT_JSON_OK;
    }
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
          .message = "Schema is false, so nothing is valid against it"};
    }
    return GTEXT_JSON_E_SCHEMA;
  }

  /*
   * `$dynamicRef` with a plain name: the target is whichever schema resource
   * *outermost* on the path here declares a `$dynamicAnchor` of that name,
   * and only if none does is the statically-resolved target used.
   *
   * This is what makes the recursive-extension pattern work. A strict schema
   * that declares `$dynamicAnchor: "node"` and then `$ref`s a permissive one
   * whose internals say `$dynamicRef: "#node"` gets its own definition back
   * at every level of the recursion, rather than the permissive one's.
   *
   * The chain runs innermost first, so the last match found walking it is the
   * outermost, which is the one wanted.
   */
  if (node->dynamic_ref_name) {
    const json_schema_node * target = node->ref_target;
    for (const json_schema_scope * s = scope; s; s = s->parent) {
      json_schema_node * found = json_schema_find_dynamic_anchor(node->owner,
          s->resource_slot, node->dynamic_ref_name, node->dynamic_ref_name_len);
      if (found) {
        target = found;
      }
    }
    if (target) {
      GTEXT_JSON_Status status = json_schema_validate_inplace(
          target, instance, depth + 1, eval, scope, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }
  }
  // $ref applies the referenced schema.  In 2020-12 a $ref sits alongside
  // other keywords and all of them apply, which is what happens here: the
  // reference is checked and then the rest of this node continues.
  else if (node->ref_target) {
    GTEXT_JSON_Status status = json_schema_validate_inplace(
        node->ref_target, instance, depth + 1, eval, scope, err);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }

  // Applicators run before the local assertions.  They can reject an instance
  // the local keywords would accept, and putting them first means a schema
  // that is only applicators still says something.
  //
  // Errors from inside a subschema are deliberately not propagated verbatim:
  // "anyOf failed" is the truth, while the last branch's complaint would name
  // a constraint the instance was never required to satisfy.
  if (node->all_of_count > 0) {
    for (size_t i = 0; i < node->all_of_count; i++) {
      GTEXT_JSON_Error sub;
      memset(&sub, 0, sizeof(sub));
      if (json_schema_validate_inplace(
              node->all_of[i], instance, depth + 1, eval, scope, &sub)
          != GTEXT_JSON_OK) {
        gtext_json_error_free(&sub);
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "Value does not match every schema in allOf"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
      gtext_json_error_free(&sub);
    }
  }

  if (node->any_of_count > 0) {
    int matched = 0;
    for (size_t i = 0; i < node->any_of_count; i++) {
      GTEXT_JSON_Error sub;
      memset(&sub, 0, sizeof(sub));
      if (json_schema_validate_inplace(
              node->any_of[i], instance, depth + 1, eval, scope, &sub)
          == GTEXT_JSON_OK) {
        matched = 1;
      }
      gtext_json_error_free(&sub);
      /* Every branch that matches contributes what it evaluated, so the
       * later ones are run even once one has matched - but only when
       * somebody is collecting, since otherwise the verdict is settled and
       * the rest is wasted work. */
      if (matched && (!eval || !eval->marks)) {
        break;
      }
    }
    if (!matched) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
            .message = "Value does not match any schema in anyOf"};
      }
      return GTEXT_JSON_E_SCHEMA;
    }
  }

  if (node->one_of_count > 0) {
    size_t matches = 0;
    for (size_t i = 0; i < node->one_of_count; i++) {
      GTEXT_JSON_Error sub;
      memset(&sub, 0, sizeof(sub));
      if (json_schema_validate_inplace(
              node->one_of[i], instance, depth + 1, eval, scope, &sub)
          == GTEXT_JSON_OK) {
        matches++;
      }
      gtext_json_error_free(&sub);
      /* No early exit on the second match: oneOf is "exactly one", and
       * stopping at two would still be the same verdict but a different
       * count if this is ever reported. */
    }
    if (matches != 1) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
            .message = matches == 0
                ? "Value does not match any schema in oneOf"
                : "Value matches more than one schema in oneOf"};
      }
      return GTEXT_JSON_E_SCHEMA;
    }
  }

  if (node->not_schema) {
    GTEXT_JSON_Error sub;
    memset(&sub, 0, sizeof(sub));
    /* `not` contributes no annotations: it succeeds exactly when its
     * subschema failed, and a subschema that failed evaluated nothing. */
    GTEXT_JSON_Status inner = json_schema_validate_depth(
        node->not_schema, instance, depth + 1, NULL, scope, &sub);
    gtext_json_error_free(&sub);
    if (inner == GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
            .message = "Value matches a schema it must not match"};
      }
      return GTEXT_JSON_E_SCHEMA;
    }
  }

  // if / then / else.  `if` asserts nothing on its own - it only selects.  An
  // absent branch means "no constraint", not "fail".
  if (node->if_schema) {
    GTEXT_JSON_Error sub;
    memset(&sub, 0, sizeof(sub));
    /* A passing `if` contributes what it evaluated; a failing one does not,
     * which is the same rule as everywhere else and is why this cannot just
     * pass `eval` in and ignore the result. */
    int cond = json_schema_validate_inplace(
                   node->if_schema, instance, depth + 1, eval, scope, &sub)
        == GTEXT_JSON_OK;
    gtext_json_error_free(&sub);
    const json_schema_node * branch =
        cond ? node->then_schema : node->else_schema;
    if (branch) {
      GTEXT_JSON_Error berr;
      memset(&berr, 0, sizeof(berr));
      if (json_schema_validate_inplace(branch, instance, depth + 1, eval, scope, &berr)
          != GTEXT_JSON_OK) {
        gtext_json_error_free(&berr);
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = cond ? "Value does not match the then schema"
                              : "Value does not match the else schema"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
      gtext_json_error_free(&berr);
    }
  }

  // Check const first (most restrictive)
  if (node->const_value) {
    if (!json_value_equal(instance, node->const_value)) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
            .message = "Value does not match const"};
      }
      return GTEXT_JSON_E_SCHEMA;
    }
    // If const matches, all other checks pass
    return GTEXT_JSON_OK;
  }

  // Check enum
  if (node->has_enum) {
    int found = 0;
    for (size_t i = 0; i < node->enum_count; i++) {
      if (json_value_equal(instance, node->enum_values[i])) {
        found = 1;
        break;
      }
    }
    if (!found) {
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = GTEXT_JSON_E_SCHEMA, .message = "Value is not in enum"};
      }
      return GTEXT_JSON_E_SCHEMA;
    }
  }

  // Check type
  if (node->type_flags != 0) {
    unsigned int instance_flag = 0;
    switch (instance->type) {
    case GTEXT_JSON_NULL:
      instance_flag = JSON_SCHEMA_TYPE_NULL;
      break;
    case GTEXT_JSON_BOOL:
      instance_flag = JSON_SCHEMA_TYPE_BOOL;
      break;
    case GTEXT_JSON_NUMBER:
      instance_flag = JSON_SCHEMA_TYPE_NUMBER;
      /* "integer" is a constraint on the value, not a separate JSON type, so
       * a whole number satisfies both "number" and "integer".  A schema
       * saying {"type":"integer"} therefore matches 5 and 5.0 but not 5.5,
       * which is what JSON Schema requires. */
      if (node->integer_excludes_fraction
              ? json_schema_is_written_as_integer(instance)
              : json_schema_is_integral(instance)) {
        instance_flag |= JSON_SCHEMA_TYPE_INTEGER;
      }
      break;
    case GTEXT_JSON_STRING:
      instance_flag = JSON_SCHEMA_TYPE_STRING;
      break;
    case GTEXT_JSON_ARRAY:
      instance_flag = JSON_SCHEMA_TYPE_ARRAY;
      break;
    case GTEXT_JSON_OBJECT:
      instance_flag = JSON_SCHEMA_TYPE_OBJECT;
      break;
    }

    if ((node->type_flags & instance_flag) == 0) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
            .message = "Value type does not match schema type"};
      }
      return GTEXT_JSON_E_SCHEMA;
    }
  }

  // Type-specific validations
  switch (instance->type) {
  case GTEXT_JSON_NUMBER: {
    // Check numeric constraints
    double num_val;
    if (gtext_json_get_double(instance, &num_val) == GTEXT_JSON_OK) {
      if (node->has_minimum && num_val < node->minimum) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "Number is less than minimum"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
      if (node->has_maximum && num_val > node->maximum) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "Number is greater than maximum"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
    }
    if (node->has_exclusive_minimum || node->has_exclusive_maximum
        || node->has_multiple_of) {
      double v = 0.0;
      if (gtext_json_get_double(instance, &v) == GTEXT_JSON_OK) {
        if (node->has_exclusive_minimum && !(v > node->exclusive_minimum)) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
                .message = "Number is not greater than exclusiveMinimum"};
          }
          return GTEXT_JSON_E_SCHEMA;
        }
        if (node->has_exclusive_maximum && !(v < node->exclusive_maximum)) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
                .message = "Number is not less than exclusiveMaximum"};
          }
          return GTEXT_JSON_E_SCHEMA;
        }
        if (node->has_multiple_of && !json_schema_is_multiple_of(node, instance, v)) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
                .message = "Number is not a multiple of multipleOf"};
          }
          return GTEXT_JSON_E_SCHEMA;
        }
      }
    }
    break;
  }

  case GTEXT_JSON_STRING: {
    /* Two lengths, and they are not interchangeable.  `minLength` and
     * `maxLength` count characters; `pattern` is handed bytes, because that
     * is what the provider's vtable promises it.  Sharing one variable
     * between them is how this arm came to measure both in bytes. */
    size_t byte_len = instance->as.string.len;
    if (node->has_min_length || node->has_max_length) {
      size_t char_len =
          json_schema_string_length(instance->as.string.data, byte_len);
      if (node->has_min_length && char_len < node->min_length) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "String is shorter than minLength"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
      if (node->has_max_length && char_len > node->max_length) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "String is longer than maxLength"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
    }

    /* `format`, when the caller asked for it to assert. A node only carries
     * a name if the compiler decided it could be checked, so there is no
     * "unknown format" branch here - an unknown one was ignored and a known
     * but uncheckable one was refused. */
    if (node->format_name) {
      int ok;
      if (node->format_name_len == 5
          && memcmp(node->format_name, "regex", 5) == 0) {
        /* The instance is itself a pattern, so the question is whether the
         * caller's engine will take it. Compiled and discarded: nothing here
         * will ever match against it. */
        char message[256];
        size_t offset = 0;
        void * compiled = NULL;
        const GTEXT_JSON_Regex_Provider * provider = node->regex_provider;
        ok = provider->compile_fn(provider->ctx, instance->as.string.data,
                 byte_len, &compiled, message, sizeof(message), &offset)
            == 0;
        if (ok) {
          provider->free_fn(provider->ctx, compiled);
        }
      }
      else {
        ok = json_format_check(node->format_name, node->format_name_len,
            instance->as.string.data, byte_len);
      }
      if (!ok) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "String does not match format"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
    }

    /* `pattern` searches: the expression need only match somewhere in the
     * string (JSON Schema core section 6.4), so a validator that anchored
     * would reject instances the specification accepts.  The anchoring is the
     * provider's obligation; nothing here can check it, which is why the
     * vtable's documentation says it twice. */
    if (node->pattern_regex) {
      int matched = 0;
      GTEXT_JSON_Status status = json_schema_regex_search(node,
          node->pattern_regex, instance->as.string.data, byte_len, &matched,
          err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
      if (!matched) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "String does not match pattern"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
    }
    break;
  }

  case GTEXT_JSON_ARRAY: {
    // Check array size constraints
    size_t arr_size = gtext_json_array_size(instance);
    if (node->has_min_items && arr_size < node->min_items) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
            .message = "Array has fewer items than minItems"};
      }
      return GTEXT_JSON_E_SCHEMA;
    }
    if (node->has_max_items && arr_size > node->max_items) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
            .message = "Array has more items than maxItems"};
      }
      return GTEXT_JSON_E_SCHEMA;
    }

    if (node->has_unique_items && node->unique_items && arr_size > 1) {
      /* Pairwise, using the same equality the enum and const keywords use.
       * Quadratic, which is fine for the array sizes a schema plausibly
       * constrains and avoids inventing a hash over arbitrary JSON values
       * whose equality is structural. */
      for (size_t i = 0; i < arr_size; i++) {
        const GTEXT_JSON_Value * a = gtext_json_array_get(instance, i);
        for (size_t j = i + 1; j < arr_size; j++) {
          const GTEXT_JSON_Value * b = gtext_json_array_get(instance, j);
          if (json_value_equal(a, b)) {
            if (err) {
              *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
                  .message = "Array has duplicate items but uniqueItems is set"};
            }
            return GTEXT_JSON_E_SCHEMA;
          }
        }
      }
    }

    // prefixItems constrains by position; anything past the last of them is
    // additional_items' business.  draft-07's `items: [...]` compiles to the
    // same place, so both spellings behave alike.
    for (size_t i = 0; i < node->prefix_items_count && i < arr_size; i++) {
      const GTEXT_JSON_Value * item = gtext_json_array_get(instance, i);
      if (!item) {
        continue;
      }
      GTEXT_JSON_Status status = json_schema_validate_depth(
          node->prefix_items[i], item, depth + 1, NULL, scope, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
      json_schema_eval_mark(eval, i);
    }
    if (node->additional_items && arr_size > node->prefix_items_count) {
      for (size_t i = node->prefix_items_count; i < arr_size; i++) {
        const GTEXT_JSON_Value * item = gtext_json_array_get(instance, i);
        if (!item) {
          continue;
        }
        GTEXT_JSON_Status status = json_schema_validate_depth(
            node->additional_items, item, depth + 1, NULL, scope, err);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
        json_schema_eval_mark(eval, i);
      }
    }

    // contains: at least one item must match, unless minContains says
    // otherwise.  minContains of 0 makes an empty array valid, which is the
    // one case where "contains" asserts nothing.
    if (node->contains_schema) {
      size_t matches = 0;
      for (size_t i = 0; i < arr_size; i++) {
        const GTEXT_JSON_Value * item = gtext_json_array_get(instance, i);
        if (!item) {
          continue;
        }
        GTEXT_JSON_Error sub;
        memset(&sub, 0, sizeof(sub));
        if (json_schema_validate_depth(
                node->contains_schema, item, depth + 1, NULL, scope, &sub)
            == GTEXT_JSON_OK) {
          matches++;
          /* `contains` evaluates the items it matched and no others, which
           * is the one applicator whose annotation depends on the instance
           * rather than on the schema's shape. */
          json_schema_eval_mark(eval, i);
        }
        gtext_json_error_free(&sub);
      }
      size_t need = node->has_min_contains ? node->min_contains : 1;
      if (matches < need) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "Array does not contain enough matching items"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
      if (node->has_max_contains && matches > node->max_contains) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "Array contains more matching items than maxContains"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
    }

    // Validate items if schema is provided
    if (node->items_schema) {
      for (size_t i = 0; i < arr_size; i++) {
        const GTEXT_JSON_Value * item = gtext_json_array_get(instance, i);
        if (!item) {
          continue;
        }

        GTEXT_JSON_Status status = json_schema_validate_depth(
            node->items_schema, item, depth + 1, NULL, scope, err);
        if (status != GTEXT_JSON_OK) {
          return status;
        }
        json_schema_eval_mark(eval, i);
      }
    }
    break;
  }

  case GTEXT_JSON_OBJECT: {
    // Object size constraints
    if (node->has_min_properties || node->has_max_properties) {
      size_t count = gtext_json_object_size(instance);
      if (node->has_min_properties && count < node->min_properties) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "Object has fewer properties than minProperties"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
      if (node->has_max_properties && count > node->max_properties) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "Object has more properties than maxProperties"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
    }

    // dependentRequired: a property's presence can require others.
    for (size_t i = 0; i < node->dep_required_count; i++) {
      const json_schema_dep_required * dep = &node->dep_required[i];
      if (!gtext_json_object_get(instance, dep->key, strlen(dep->key))) {
        continue; // trigger absent, nothing required
      }
      for (size_t j = 0; j < dep->required_count; j++) {
        const char * name = dep->required[j];
        if (!gtext_json_object_get(instance, name, strlen(name))) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
                .message = "A property required by dependentRequired is "
                           "missing"};
          }
          return GTEXT_JSON_E_SCHEMA;
        }
      }
    }

    // Check required properties
    for (size_t i = 0; i < node->required_count; i++) {
      const char * req_key = node->required_keys[i];
      size_t req_key_len = strlen(req_key);
      const GTEXT_JSON_Value * prop_val =
          gtext_json_object_get(instance, req_key, req_key_len);
      if (!prop_val) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA,
              .message = "Required property is missing"};
        }
        return GTEXT_JSON_E_SCHEMA;
      }
    }

    // propertyNames applies to each key, as a string instance.  The key is
    // wrapped in a temporary value so the same node machinery can check it.
    if (node->property_names) {
      size_t count = gtext_json_object_size(instance);
      for (size_t i = 0; i < count; i++) {
        size_t klen = 0;
        const char * kname = gtext_json_object_key(instance, i, &klen);
        if (!kname) {
          continue;
        }
        /* On the schema's allocator: gtext_json_new_string() would make its
           own context in the C library, once per key of every object this
           keyword checks, and the gate cannot see a bypass that is a call. */
        GTEXT_JSON_Value * key_val = json_value_new_string_on(kname, klen,
            node->owner && node->owner->ctx ? node->owner->ctx->alloc : NULL);
        if (!key_val) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory checking propertyNames"};
          }
          return GTEXT_JSON_E_OOM;
        }
        GTEXT_JSON_Status status = json_schema_validate_depth(
            node->property_names, key_val, depth + 1, NULL, scope, err);
        gtext_json_free(key_val);
        if (status != GTEXT_JSON_OK) {
          if (err && err->code == GTEXT_JSON_E_SCHEMA) {
            err->message = "A property name does not match propertyNames";
          }
          return status;
        }
      }
    }

    /* patternProperties and additionalProperties, in one pass over the
     * instance's properties.
     *
     * They are here together because the second is defined in terms of the
     * first: additionalProperties applies to every property that neither
     * `properties` named nor any `patternProperties` expression matched.
     * Two passes would mean running each regular expression over each
     * property name twice, and - the reason that matters - would mean two
     * places that have to agree on what "matched" means.
     *
     * A property can match several patterns, and then every one of their
     * schemas applies. */
    if (node->pattern_properties_count > 0 || node->additional_properties) {
      size_t count = gtext_json_object_size(instance);
      for (size_t i = 0; i < count; i++) {
        size_t klen = 0;
        const char * kname = gtext_json_object_key(instance, i, &klen);
        if (!kname) {
          continue;
        }
        const GTEXT_JSON_Value * pv = gtext_json_object_value(instance, i);
        if (!pv) {
          continue;
        }

        int covered = 0;
        for (size_t p = 0; p < node->properties_count && !covered; p++) {
          if (node->properties[p].key_len == klen
              && memcmp(node->properties[p].key, kname, klen) == 0) {
            covered = 1;
          }
        }

        for (size_t p = 0; p < node->pattern_properties_count; p++) {
          const json_schema_pattern_property * entry =
              &node->pattern_properties[p];
          int matched = 0;
          GTEXT_JSON_Status status = json_schema_regex_search(
              node, entry->regex, kname, klen, &matched, err);
          if (status != GTEXT_JSON_OK) {
            return status;
          }
          if (!matched) {
            continue;
          }
          covered = 1;
          status = json_schema_validate_depth(
              entry->schema, pv, depth + 1, NULL, scope, err);
          json_schema_eval_mark(eval, i);
          if (status != GTEXT_JSON_OK) {
            if (err && err->code == GTEXT_JSON_E_SCHEMA) {
              err->message =
                  "A property does not satisfy its patternProperties schema";
            }
            return status;
          }
        }

        if (covered || !node->additional_properties) {
          continue;
        }
        GTEXT_JSON_Status status = json_schema_validate_depth(
            node->additional_properties, pv, depth + 1, NULL, scope, err);
        json_schema_eval_mark(eval, i);
        if (status != GTEXT_JSON_OK) {
          if (err && err->code == GTEXT_JSON_E_SCHEMA) {
            err->message = "A property is not allowed by additionalProperties";
          }
          return status;
        }
      }
    }

    // dependentSchemas: a property's presence applies a whole schema.
    for (size_t i = 0; i < node->dep_schemas_count; i++) {
      const json_schema_dep_schema * dep = &node->dep_schemas[i];
      if (!gtext_json_object_get(instance, dep->key, strlen(dep->key))) {
        continue;
      }
      GTEXT_JSON_Status status = json_schema_validate_inplace(
          dep->schema, instance, depth + 1, eval, scope, err);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }

    /* Validate properties.
     *
     * Walked over the instance rather than over the schema when annotations
     * are being collected, because what has to be recorded is the *instance*
     * position each named property sits at, and a lookup by key gives back a
     * value with no position attached. */
    if (node->properties_count > 0) {
      if (eval && eval->marks) {
        size_t count = gtext_json_object_size(instance);
        for (size_t i = 0; i < count; i++) {
          size_t klen = 0;
          const char * kname = gtext_json_object_key(instance, i, &klen);
          const GTEXT_JSON_Value * prop_val =
              gtext_json_object_value(instance, i);
          if (!kname || !prop_val) {
            continue;
          }
          for (size_t p = 0; p < node->properties_count; p++) {
            const json_schema_property * prop = &node->properties[p];
            if (prop->key_len != klen || memcmp(prop->key, kname, klen) != 0) {
              continue;
            }
            GTEXT_JSON_Status status = json_schema_validate_depth(
                prop->schema, prop_val, depth + 1, NULL, scope, err);
            if (status != GTEXT_JSON_OK) {
              return status;
            }
            json_schema_eval_mark(eval, i);
            break;
          }
        }
      }
      else {
        for (size_t i = 0; i < node->properties_count; i++) {
          const json_schema_property * prop = &node->properties[i];
          const GTEXT_JSON_Value * prop_val =
              gtext_json_object_get(instance, prop->key, prop->key_len);

          if (prop_val) {
            // Property exists, validate it
            GTEXT_JSON_Status status = json_schema_validate_depth(
                prop->schema, prop_val, depth + 1, NULL, scope, err);
            if (status != GTEXT_JSON_OK) {
              return status;
            }
          }
        }
      }
    }
    break;
  }

  default:
    break;
  }

  return GTEXT_JSON_OK;
}

// Public API functions

GTEXT_API GTEXT_JSON_Schema_Options gtext_json_schema_options_default(void) {
  GTEXT_JSON_Schema_Options opts;
  /*
   * Zeroed wholesale rather than field by field, because every default here
   * is the zero value - no relaxation, no provider, `format` an annotation -
   * and because assigning them one at a time is the kind of correct that
   * stops being correct the moment somebody adds a field.  It was that
   * spelling when `format` was added, and a caller's stack would have decided
   * whether `format` asserted.
   */
  memset(&opts, 0, sizeof(opts));
  return opts;
}

GTEXT_API GTEXT_JSON_Schema * gtext_json_schema_compile(
    const GTEXT_JSON_Value * schema_doc, GTEXT_JSON_Error * err) {
  return gtext_json_schema_compile_with_options(schema_doc, NULL, err);
}

GTEXT_API GTEXT_JSON_Schema * gtext_json_schema_compile_with_options(
    const GTEXT_JSON_Value * schema_doc,
    const GTEXT_JSON_Schema_Options * opts_in, GTEXT_JSON_Error * err) {
  GTEXT_JSON_Schema_Options defaults = gtext_json_schema_options_default();
  const GTEXT_JSON_Schema_Options * opts = opts_in ? opts_in : &defaults;
  /*
   * Read once, here, and then carried on the schema's own context rather than
   * on the options structure - which the caller is not promised outlives the
   * compiled schema, and usually does not.  Everything from here on reaches it
   * as `schema->ctx->alloc` or `cc->ctx->alloc`, both of which are this.
   */
  const GTEXT_Allocator * alloc = opts->allocator;

  if (!schema_doc) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Schema document must not be NULL"};
    }
    return NULL;
  }

  /* A boolean is a schema too - "true" accepts everything and "false"
   * accepts nothing - and that is as true at the root as it is for a
   * subschema. */
  if (schema_doc->type != GTEXT_JSON_OBJECT
      && schema_doc->type != GTEXT_JSON_BOOL) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Schema document must be an object or a boolean"};
    }
    return NULL;
  }

  /* A provider missing one of its three functions is a caller error, not a
   * schema that has no regular-expression support: the difference matters,
   * because the second silently refuses every `pattern` and the first is a
   * bug in the adapter that would otherwise surface as a null call much
   * later. */
  if (opts->regex
      && (!opts->regex->compile_fn || !opts->regex->search_fn
          || !opts->regex->free_fn)) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "A regular-expression provider must supply compile_fn, "
                     "search_fn and free_fn"};
    }
    return NULL;
  }

  // Allocate schema structure
  GTEXT_JSON_Schema * schema = (GTEXT_JSON_Schema *)gtext_allocator_calloc(
      alloc, 1, sizeof(GTEXT_JSON_Schema));
  if (!schema) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory allocating schema"};
    }
    return NULL;
  }

  /*
   * The context for cloned enum/const values, and the one place the caller's
   * allocator is recorded for the schema's whole life.  json_context::alloc is
   * documented as never NULL, so everything downstream can read it from
   * `cc->ctx` or `schema->ctx` and nothing has to default it again.  This used
   * to be json_context_new(NULL), which put every `enum` and `const` value a
   * schema holds in the C library whatever the caller had asked for.
   */
  schema->ctx = json_context_new(alloc);
  if (!schema->ctx) {
    gtext_allocator_free(alloc, schema);
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory creating schema context"};
    }
    return NULL;
  }

  // Allocate root node
  schema->root = (json_schema_node *)gtext_allocator_calloc(alloc, 1, sizeof(json_schema_node));
  if (!schema->root) {
    json_context_free(schema->ctx);
    gtext_allocator_free(alloc, schema);
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory allocating schema node"};
    }
    return NULL;
  }

  /* The document is cloned so that $ref can be resolved against it after the
   * caller frees theirs.  A schema with no $ref pays for this too; keeping
   * two code paths, one of which only some schemas exercise, is the more
   * expensive choice in the end.
   *
   * On *this* schema's allocator and not the document's, which is what
   * gtext_json_clone() would have given it.  The clone outlives the document
   * by contract - that is the whole reason it exists - so it cannot be held in
   * an allocator chosen for the document's lifetime.  It keeps a context of its
   * own all the same, so gtext_json_free() below still releases it. */
  schema->doc = json_value_clone_new_context(schema_doc, alloc);
  if (!schema->doc) {
    json_schema_node_free(schema->root, alloc);
    json_context_free(schema->ctx);
    gtext_allocator_free(alloc, schema);
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory cloning the schema document"};
    }
    return NULL;
  }

  /* The provider is copied rather than pointed at: the options structure is
   * the caller's automatic variable as often as not, and the compiled
   * patterns outlive the call that made them.  What `ctx` points at is still
   * the caller's to keep alive, and the header says so. */
  if (opts->regex) {
    schema->regex_provider = *opts->regex;
    schema->has_regex_provider = 1;
  }

  /* The base the document is compiled against: what the caller said it was
   * retrieved from, or nothing. A root `$id` is resolved against this, which
   * is what makes a relative one mean anything. */
  schema->base_uri = json_uri_without_fragment(
      opts->base_uri ? opts->base_uri : "",
      opts->base_uri ? strlen(opts->base_uri) : 0, alloc);
  if (!schema->base_uri) {
    gtext_json_schema_free(schema);
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory recording the base URI"};
    }
    return NULL;
  }

  /*
   * The dialect an undeclared document is read as.  2020-12 unless the caller
   * says otherwise; a `$schema` inside the document still wins, per resource.
   *
   * An unreadable name is refused rather than ignored, for the reason the
   * whole engine refuses rather than ignores: a caller who passed a dialect
   * this library cannot read and got a compile back would have been told the
   * opposite of the truth.
   */
  json_schema_draft initial_draft = JSON_DRAFT_2020_12;
  if (opts && opts->default_dialect) {
    initial_draft = json_schema_draft_for_uri(
        opts->default_dialect, strlen(opts->default_dialect));
    if (initial_draft == 0) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_SCHEMA_UNSUPPORTED,
            .message = "default_dialect names a draft this implementation "
                       "does not read"};
      }
      gtext_json_schema_free(schema);
      return NULL;
    }
  }

  /*
   * Every `$id` and `$anchor` is found before anything is compiled. A `$ref`
   * is free to name something defined later in the document than the
   * reference itself, so resolving identifiers as they are met would answer
   * a forward reference and a backward one differently.
   *
   * It needs the dialect, which is why that is settled first: two of the three
   * things it looks for are spelled differently before 2019-09.
   */
  GTEXT_JSON_Status status = json_schema_add_resource(schema,
      json_uri_without_fragment(schema->base_uri, strlen(schema->base_uri), alloc),
      schema->doc, err, NULL);
  if (status == GTEXT_JSON_OK) {
    status = json_schema_scan_resources(
        schema, schema->doc, schema->base_uri, initial_draft, 0, err);
  }
  if (status != GTEXT_JSON_OK) {
    gtext_json_schema_free(schema);
    return NULL;
  }

  /* A root `$id` moves the document's own base, so compilation starts in
   * whichever scope the pre-pass ended up putting the root in. Spelled `id`
   * in draft-04, which is why this asks rather than assuming. */
  const char * root_base = schema->base_uri;
  const GTEXT_JSON_Value * root_id = json_schema_identifier(
      schema->doc, json_schema_draft_in(schema->doc, initial_draft));
  if (root_id && root_id->type == GTEXT_JSON_STRING) {
    for (size_t i = 0; i < schema->resources_count; i++) {
      if (schema->resources[i].value == schema->doc
          && strcmp(schema->resources[i].uri, schema->base_uri) != 0) {
        root_base = schema->resources[i].uri;
        break;
      }
    }
  }

  json_schema_compile_ctx cc = {.ctx = schema->ctx,
      .opts = opts,
      .schema = schema,
      .depth = 0,
      .vocabularies = JSON_VOCAB_DEFAULT,
      .draft = initial_draft,
      .base_uri = root_base};

  /* The root's own `$schema` is read by compile_node like any other
   * resource's, so there is no separate pass for it here. */
  status = json_schema_compile_node(schema->root, schema->doc, &cc, err);
  if (status != GTEXT_JSON_OK) {
    gtext_json_schema_free(schema);
    return NULL;
  }

  /*
   * Every `$dynamicAnchor` the pre-pass found, compiled whether or not a
   * `$ref` reaches it.
   *
   * The whole point of one is that a reference finds it at validation time
   * without naming it, so "is it referenced" is not a question that can be
   * asked at compile time. The specification's own example puts the anchor
   * in a `$defs` nothing refers to, and compiling only the reachable schemas
   * left it out - which then resolved the reference to the wrong one and
   * said nothing about having done so.
   *
   * The loop re-reads the count each time because compiling one can register
   * another, and a new entry is one more to compile.
   */
  for (size_t i = 0; i < schema->dynamic_anchors_count; i++) {
    json_schema_dynamic_anchor * entry = &schema->dynamic_anchors[i];
    if (entry->node || !entry->value || entry->resource_slot == 0) {
      continue;
    }
    size_t name_len = strlen(entry->name);
    char * ref = (char *)gtext_allocator_malloc(alloc, name_len + 2);
    if (!ref) {
      gtext_json_schema_free(schema);
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory compiling a $dynamicAnchor"};
      }
      return NULL;
    }
    ref[0] = '#';
    memcpy(ref + 1, entry->name, name_len + 1);
    json_schema_node * target = NULL;
    cc.base_uri = schema->resources[entry->resource_slot - 1].uri;
    status = json_schema_resolve_ref(&target, ref, name_len + 1, &cc, err);
    gtext_allocator_free(alloc, ref);
    if (status != GTEXT_JSON_OK) {
      gtext_json_schema_free(schema);
      return NULL;
    }
    /* `entry` may have been moved by a realloc inside the compile. */
    schema->dynamic_anchors[i].node = target;
  }

  return schema;
}

GTEXT_API void gtext_json_schema_free(GTEXT_JSON_Schema * schema) {
  if (!schema) {
    return;
  }

  /*
   * Captured before anything is released, because the allocator lives on
   * schema->ctx and json_context_free() below frees that - after which
   * schema->ctx->alloc is a read of freed memory, and `gtext_allocator_free(alloc, schema)` would be
   * asking an allocator that no longer exists to release the object holding
   * it.
   */
  const GTEXT_Allocator * alloc = schema->ctx ? schema->ctx->alloc : NULL;

  json_schema_node_free(schema->root, alloc);

  /* The registry owns every $ref target, so they are freed here rather than
   * by the nodes that refer to them. */
  if (schema->refs) {
    for (size_t i = 0; i < schema->refs_count; i++) {
      gtext_allocator_free(alloc, schema->refs[i].uri);
      json_schema_node_free(schema->refs[i].node, alloc);
    }
    gtext_allocator_free(alloc, schema->refs);
  }

  if (schema->resources) {
    for (size_t i = 0; i < schema->resources_count; i++) {
      gtext_allocator_free(alloc, schema->resources[i].uri);
    }
    gtext_allocator_free(alloc, schema->resources);
  }
  /* After `resources`, which borrows pointers into these. Slots for
   * documents no reference reached are NULL, which gtext_json_free() takes. */
  if (schema->embedded) {
    for (size_t i = 0; i < schema->embedded_count; i++) {
      gtext_json_free(schema->embedded[i]);
    }
    gtext_allocator_free(alloc, schema->embedded);
  }
  if (schema->dynamic_anchors) {
    for (size_t i = 0; i < schema->dynamic_anchors_count; i++) {
      gtext_allocator_free(alloc, schema->dynamic_anchors[i].name);
    }
    gtext_allocator_free(alloc, schema->dynamic_anchors);
  }
  gtext_allocator_free(alloc, schema->base_uri);

  gtext_json_free(schema->doc);
  json_context_free(schema->ctx);
  gtext_allocator_free(alloc, schema);
}

GTEXT_API GTEXT_JSON_Status gtext_json_schema_validate(
    const GTEXT_JSON_Schema * schema, const GTEXT_JSON_Value * instance,
    GTEXT_JSON_Error * err) {
  if (!schema || !instance) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Schema and instance must not be NULL"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  return json_schema_validate_node(schema->root, instance, err);
}
