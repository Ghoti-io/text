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
 * @file yaml_schema.h
 * @brief Validate a YAML document against a JSON Schema.
 *
 * @anchor yaml_schema_module
 *
 * ## There is no YAML schema language
 *
 * YAML 1.2 section 10 defines *schemas* - failsafe, JSON, core - and they are
 * about implicit typing: whether the plain scalar `yes` is a boolean and
 * whether `0o14` is an integer. ::GTEXT_YAML_Schema selects one of those, and
 * it is not a validator. Kwalify and Rx were proposed as validation languages
 * and neither is in use.
 *
 * What is in use is JSON Schema. Kubernetes, OpenAPI, GitHub Actions, Azure
 * Pipelines, Ansible and the `yaml-language-server` directive all describe
 * YAML documents with it, and the schemas themselves are usually written in
 * YAML. So that is what this validates against, rather than inventing a
 * fourth proposal: the engine is \ref json_schema_module "the JSON Schema
 * one", with every draft it supports.
 *
 * ## What this adds over converting and validating by hand
 *
 * A caller can already write
 *
 * ```c
 * GTEXT_JSON_Value * j = NULL;
 * gtext_yaml_to_json(doc, &j, &yerr);
 * gtext_json_schema_validate(schema, j, &jerr);
 * ```
 *
 * and get the right verdict. What it does not get is **a position in the YAML
 * file**. A JSON Schema failure names a place in the JSON instance, and the
 * JSON instance is a thing the user never wrote; a message about `/spec/2/env`
 * with no line number sends them hunting through a file that does not have
 * those names in it.
 *
 * gtext_yaml_validate() resolves that pointer back through the YAML document
 * and reports the line and column of the node it names, so a failure points at
 * the user's own text. It also keeps the conversion's refusals - a document
 * JSON cannot represent at all - distinct from a schema's, which is a
 * difference the two-call form flattens into "it did not validate".
 *
 * ## What the conversion does to the document first
 *
 * Validation is of the *converted* document, so everything
 * gtext_yaml_to_json() does applies, and three of those are worth knowing
 * before trusting a pass:
 *
 * - **Non-string keys.** JSON names are strings. By default a YAML mapping
 *   with a non-string key is refused; with
 *   ::GTEXT_YAML_To_JSON_Options::coerce_keys_to_strings the key `1` becomes
 *   the name `"1"`, and a schema's `properties` has to spell it that way.
 * - **Aliases.** Refused by default. With
 *   ::GTEXT_YAML_To_JSON_Options::allow_resolved_aliases an alias is expanded,
 *   so a schema sees the same subtree twice and `maxItems` and friends count
 *   the expansion. A failure in an expanded copy is reported at the *anchor*,
 *   which is the only place in the file that value is written.
 * - **Tags.** `!!timestamp`, `!!binary`, `!!set` and `!!omap` become the JSON
 *   shapes gtext_yaml_to_json() gives them; a schema describes those shapes
 *   and not the YAML tags.
 *
 * One document at a time. A multi-document stream is validated by walking it
 * with gtext_yaml_document_at() and validating each.
 */

#ifndef GHOTI_IO_GTEXT_YAML_YAML_SCHEMA_H
#define GHOTI_IO_GTEXT_YAML_YAML_SCHEMA_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/json/json_schema.h>
#include <ghoti.io/text/yaml/yaml_core.h>
#include <ghoti.io/text/yaml/yaml_dom.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct GTEXT_YAML_Validate_Options
 * @brief How a YAML document becomes a JSON instance before validation.
 */
typedef struct {
  /**
   * The conversion. Its defaults are gtext_yaml_to_json_options_default()'s,
   * which refuse a document JSON cannot represent rather than reshaping one
   * silently.
   */
  GTEXT_YAML_To_JSON_Options convert;
} GTEXT_YAML_Validate_Options;

/*
 * There is no allocator here, and that is not an omission. The converted
 * instance is built and freed inside the call by gtext_yaml_to_json(), which
 * takes its memory from the document being converted - so a validation already
 * runs on the allocator the caller gave the parse. The walk that locates a
 * failure is on the stack, and the one allocation left is the pointer string
 * the JSON error carries, which this frees before returning.
 */

/**
 * @brief Defaults for gtext_yaml_validate().
 */
GTEXT_API GTEXT_YAML_Validate_Options gtext_yaml_validate_options_default(void);

/**
 * @brief Compile a schema that is written in YAML.
 *
 * Most schemas for YAML documents are themselves YAML, so this converts
 * @p schema_doc and compiles the result. It is here because writing the two
 * calls out means handling two error types for one failure, and because the
 * conversion has to be given the same options the instance's will be - a
 * schema whose `properties` names a coerced key has to be converted with
 * coercion on as well, or the names will not match.
 *
 * The result is an ordinary compiled schema: free it with
 * gtext_json_schema_free(). The converted JSON is not kept, because
 * gtext_json_schema_compile() clones the schema document for its own `$ref`
 * resolution and is independent of the value it was handed.
 *
 * @param schema_doc The schema, as a YAML document (must not be NULL)
 * @param opts Conversion options, or NULL for the defaults
 * @param err Receives the reason on failure, or NULL
 * @return A compiled schema to free with gtext_json_schema_free(), or NULL
 */
GTEXT_API GTEXT_JSON_Schema * gtext_yaml_schema_compile(
    const GTEXT_YAML_Document * schema_doc,
    const GTEXT_YAML_Validate_Options * opts, GTEXT_YAML_Error * err);

/**
 * @brief Validate a YAML document against a compiled JSON Schema.
 *
 * ## What the three outcomes mean
 *
 * - ::GTEXT_YAML_OK - the document converts and satisfies the schema.
 * - ::GTEXT_YAML_E_SCHEMA - it converts and does not satisfy the schema.
 *   @p err carries the schema engine's message and **the line and column of
 *   the YAML node the failure is about**, which is the point of this function.
 * - anything else - the document could not be converted at all, and nothing
 *   has been said about the schema. The message is the conversion's.
 *
 * The distinction matters: "this YAML cannot be expressed as JSON" and "this
 * YAML is not what the schema asks for" are different problems with different
 * fixes, and a caller doing the conversion itself has to remember to keep them
 * apart.
 *
 * ## Where the position comes from, and when there is none
 *
 * The schema engine reports an RFC 6901 pointer into the converted instance.
 * That pointer is walked against the YAML document - a name against a mapping
 * key, an index against a sequence entry - and the node it lands on supplies
 * the location. Mapping keys are spelled the way the conversion spelled them,
 * so a coerced key is found by its coerced name.
 *
 * @p err's line and column are left at 0 in the cases where there is nothing
 * to point at:
 *
 * - the failure is about the document as a whole (`required` on the root, for
 *   instance), where the pointer is the empty string and the root node's own
 *   location is reported instead;
 * - the failure is about a *property name* rather than a value, which
 *   `propertyNames` produces and for which JSON Schema has no pointer;
 * - the document was parsed without source locations retained, in which case
 *   no node in it has one;
 * - the pointer is longer than 512 bytes, which this resolves on the stack.
 *   That is about sixty nested names; a document nested that deep with keys
 *   that long is not one where a line number is what stands between the user
 *   and the problem.
 *
 * @param doc Document to validate (must not be NULL)
 * @param schema Compiled schema (must not be NULL)
 * @param opts Options, or NULL for the defaults
 * @param err Receives the failure, or NULL. Nothing in it needs freeing.
 * @return GTEXT_YAML_OK, GTEXT_YAML_E_SCHEMA, or a conversion status
 */
GTEXT_API GTEXT_YAML_Status gtext_yaml_validate(
    const GTEXT_YAML_Document * doc, const GTEXT_JSON_Schema * schema,
    const GTEXT_YAML_Validate_Options * opts, GTEXT_YAML_Error * err);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GTEXT_YAML_YAML_SCHEMA_H */
