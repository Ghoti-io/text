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
 * JSON Schema validation (core subset).
 *
 * This header provides functions for compiling and validating JSON values
 * against JSON Schema documents. This implementation supports a pragmatic
 * core subset of JSON Schema features.
 *
 * The engine enforces a core subset, and **refuses schemas it cannot fully
 * enforce**. A schema using a standard keyword from the list of unsupported
 * keywords below fails to compile with GTEXT_JSON_E_SCHEMA_UNSUPPORTED and
 * the offending keyword named in the error's context_snippet. This is
 * deliberate: silently ignoring an assertion keyword makes invalid data
 * validate clean, which is worse than refusing the schema outright.
 *
 * Genuinely unknown keywords - vendor extensions, and the annotation
 * keywords title, description, default, examples, $comment, readOnly,
 * writeOnly and deprecated - are ignored, as JSON Schema requires. So is a
 * keyword from a vocabulary the schema's dialect does not use: $vocabulary
 * in the metaschema $schema names decides that, and a vocabulary declared
 * required that this engine does not have is refused. So are
 * contentEncoding, contentMediaType and contentSchema, which 2020-12 defines
 * as annotations rather than assertions, and format, which is an annotation
 * unless GTEXT_JSON_Schema_Options::format asks for it to be asserted. So are
 * $schema and $vocabulary, which choose the dialect rather than asserting
 * anything about the instance. $id, $defs, definitions and $anchor are not in
 * this list any more: they used to be ignored because nothing read them "while
 * $ref is unsupported", and $ref is supported, so they are load-bearing parts
 * of the reference model described below.
 *
 * Callers that genuinely want the old behavior can set
 * allow_unsupported_keywords in GTEXT_JSON_Schema_Options and compile with
 * gtext_json_schema_compile_with_options().
 *
 * Supported schema keywords:
 * - type: null, boolean, object, array, number, integer, string - and arrays
 *   of those. "integer" constrains the value rather than naming a distinct
 *   JSON type, so 5 and 5.0 satisfy it and 5.5 does not.
 * - properties: Object property schemas (recursive validation)
 * - required: List of required property names
 * - items: Array item schema (all items must match)
 * - enum: Array of allowed values (exact match). An empty array is a
 *   schema no instance satisfies, not a schema that asserts nothing
 * - const: Single allowed value (exact match)
 * - minimum/maximum: Numeric constraints
 * - minLength/maxLength: String length constraints
 * - minItems/maxItems: Array size constraints
 * - uniqueItems: Array elements must be pairwise distinct, by structural
 *   equality
 * - exclusiveMinimum/exclusiveMaximum: Strict numeric bounds
 * - multipleOf: Exact divisibility; the divisor must be greater than zero.
 *   Asked in decimal over the digits the document wrote, because that is
 *   the question: 0.0075 is 75 lots of 0.0001, and is not a whole number
 *   of them in the binary either value rounds to. A value whose lexeme was
 *   not kept, or which needs more than 19 significant digits, falls back to
 *   binary remainder
 * - minProperties/maxProperties: Object size constraints
 * - dependentRequired: One property's presence requires others
 * - allOf/anyOf/oneOf/not: Boolean applicators. oneOf is exactly one, so two
 *   matching branches is a failure
 * - if/then/else: "if" selects rather than asserts; an absent branch is no
 *   constraint
 * - $ref, $id, $anchor, $defs and definitions: the reference model is the
 *   specification's, built on URIs. $id establishes a base URI and an
 *   embedded resource; $anchor names a location in one; $ref is a
 *   URI-reference resolved against the base in scope, so "#", "#/...",
 *   "#name", "other.json" and an absolute URI all work. Recursive references
 *   work; targets are compiled once and shared. A reference that leaves the
 *   document is fetched through GTEXT_JSON_Schema_Options::resolver, and
 *   refused at compile time when there is none or it does not know the URI -
 *   except for the published meta-schemas of every dialect this engine reads,
 *   which are embedded, so that a schema saying "this instance is a valid
 *   schema" resolves without a resolver and without a socket. draft-04 spells
 *   the identifier "id" and the two are read only in the draft that has them
 * - prefixItems and items, where "items" applies to the elements at an
 *   index past the end of "prefixItems" and to every element when there is
 *   no "prefixItems". draft-07's array-valued "items" compiles to
 *   prefixItems and its "additionalItems" to the same slot "items" fills,
 *   so a schema written either way validates the same. A schema carrying
 *   both "additionalItems" and an "items" that follows "prefixItems" has
 *   named one slot twice, in two drafts that disagree about which wins, and
 *   is refused
 * - contains, minContains, maxContains
 * - additionalProperties, propertyNames
 * - dependentSchemas, and draft-07's "dependencies" in either of its forms
 * - Boolean schemas: "true" accepts everything and "false" nothing, anywhere
 *   a schema is allowed, the root included
 * - $dynamicRef and $dynamicAnchor: a reference whose target is the outermost
 *   schema resource on the validation path that declares an anchor of that
 *   name, which is what lets a schema extend another and have the other's
 *   internal references come back to the extension. A $dynamicRef whose
 *   fragment is not a plain name, or whose target declares no $dynamicAnchor
 *   of that name, is an ordinary $ref
 * - unevaluatedItems and unevaluatedProperties, which apply to whatever the
 *   rest of the same schema object did not reach. What counts as reached is
 *   carried across the in-place applicators - allOf, anyOf, oneOf,
 *   if/then/else, $ref, dependentSchemas - and a subschema that failed
 *   contributes nothing, so an anyOf branch that named half the properties
 *   and then failed has not evaluated them
 *
 * - pattern, patternProperties: only when the caller supplies a
 *   regular-expression provider through GTEXT_JSON_Schema_Options. Without
 *   one they remain in the unsupported list below, because this library has
 *   no regular-expression engine of its own and inventing a half one would
 *   be the silent-mis-validation failure the strict check exists to prevent
 *
 * Unsupported standard keywords (rejected at compile time):
 * - pattern, patternProperties, when no provider was supplied
 * - a $schema naming draft-03 or earlier, or the unversioned
 *   http://json-schema.org/schema# alias, which names no draft: it has been
 *   answered with whatever was current, so a document using it has not said
 *   what it was written against. draft-03 spells "required" as a boolean on
 *   each property and "divisibleBy" as what later drafts call multipleOf,
 *   which is a different language rather than the same one spelled
 *   differently. 2020-12, 2019-09, draft-07, draft-06 and draft-04 are read,
 *   each with its own keyword set, and the dialect is scoped to the resource
 *   that declares it. draft-04's three rules of its own are implemented: the
 *   identifier is "id", exclusiveMinimum and exclusiveMaximum are booleans
 *   that modify minimum and maximum, and "integer" is a constraint on how a
 *   number is written, so 1.0 is not one. A document with no $schema is read
 *   as default_dialect, or as 2020-12 when that is not set
 * - $recursiveRef with any value but "#". 2019-09 defines exactly one, and
 *   the keyword is otherwise implemented: $recursiveRef and $recursiveAnchor
 *   are 2019-09's spelling of $dynamicRef and $dynamicAnchor, answered by the
 *   same dynamic-scope walk under one anonymous anchor name
 * - regex as a `format`, when format assertion is asked for and no provider
 *   was supplied. It is the only format name this library declines; every
 *   other one in the vocabulary is checked
 *
 * Note on $ref depth: a schema that refers to itself without consuming any
 * instance, such as {"$ref":"#"}, compiles successfully and fails validation
 * with GTEXT_JSON_E_DEPTH rather than recursing without bound.
 *
 * The schema engine is designed to be modular and optional at compile time.
 */

#ifndef GHOTI_IO_GTEXT_JSON_JSON_SCHEMA_H
#define GHOTI_IO_GTEXT_JSON_JSON_SCHEMA_H

#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque schema structure
 *
 * Represents a compiled JSON Schema. The structure is opaque to users;
 * all interaction is through the API functions.
 */
typedef struct GTEXT_JSON_Schema GTEXT_JSON_Schema;

/**
 * @brief A regular-expression engine, supplied by the caller.
 *
 * JSON Schema's `pattern` and `patternProperties` are regular expressions,
 * and this library has no engine. Rather than acquire a dependency on one -
 * which every caller would then pay for, including the many who never write a
 * `pattern` - the engine arrives through this vtable, and the two keywords
 * are enforced when one is present and refused when it is not.
 *
 * Three obligations, and each is somewhere a validator can quietly get it
 * wrong:
 *
 * - **The dialect is ECMA-262 with the `u` flag.** JSON Schema core section
 *   6.4 says so. A schema author writing `\d` means ECMA-262's `\d`, which
 *   is ASCII, and not Python's or .NET's, which are not.
 * - **The match is a search, not an anchored match.** The pattern need only
 *   match *somewhere* in the string, so a provider that anchors rejects
 *   instances the specification accepts. This is the single most common
 *   mistake in the wild.
 * - **Both strings are UTF-8 and may contain a NUL.** Lengths are given;
 *   neither is terminated for you.
 *
 * `ctx` is passed to every call, so one implementation can serve several
 * independent configurations. Everything reachable from it must outlive every
 * compiled schema that used it: compiled patterns are freed when the schema
 * is, which is after the caller has stopped thinking about the provider.
 */
typedef struct {
  /**
   * User-defined, passed to each call.
   */
  void * ctx;

  /**
   * Compile one pattern. Return 0 on success, having stored an opaque handle
   * in `*out_regex`; return non-zero on failure.
   *
   * On failure, write a description into `message` - at most
   * `message_capacity` bytes including the terminator, never more - and store
   * the byte offset within the pattern at which it went wrong in
   * `*out_offset`. Both are copied out before the call returns, so neither
   * needs to outlive it: the message reaches the caller in
   * GTEXT_JSON_Error::context_snippet and the offset in
   * GTEXT_JSON_Error::offset.
   *
   * A refusal that has no position - "this pattern needs an engine whose
   * worst case is exponential" is about the whole of it - stores `(size_t)-1`
   * for the offset, and the caller sees `(size_t)-1` rather than 0, so that
   * nothing points at a character that is not the problem.
   *
   * `pattern` is UTF-8 of `pattern_len` bytes and is not NUL-terminated.
   */
  int (*compile_fn)(void * ctx, const char * pattern, size_t pattern_len,
      void ** out_regex, char * message, size_t message_capacity,
      size_t * out_offset);

  /**
   * Search `subject` for `regex`. Return a positive value if it matches
   * anywhere, 0 if it does not, and a **negative** value if the search could
   * not be completed - a step or memory limit reached, an allocation failed.
   *
   * The negative case exists because it is a third answer and not a second
   * one. A pattern that exhausts its budget has not told you the instance is
   * invalid; reporting that as "no match" turns a denial-of-service defence
   * into a wrong validation result. It surfaces as GTEXT_JSON_E_LIMIT, which
   * is neither GTEXT_JSON_OK nor GTEXT_JSON_E_SCHEMA.
   *
   * `subject` is UTF-8 of `subject_len` bytes and is not NUL-terminated.
   */
  int (*search_fn)(
      void * ctx, void * regex, const char * subject, size_t subject_len);

  /**
   * Release a handle `compile_fn` produced. Called once per handle, when the
   * compiled schema is freed. Never called with NULL.
   */
  void (*free_fn)(void * ctx, void * regex);
} GTEXT_JSON_Regex_Provider;

/**
 * @brief Where a schema from another document comes from
 *
 * A `$ref` is a URI-reference, and one that resolves outside the document
 * being compiled names a schema this library has no way to fetch. Nothing
 * here opens a socket or reads a file: the caller decides what a URI means
 * and hands back the document, which is the only arrangement in which a
 * schema compile cannot become a network request nobody asked for.
 *
 * Without a resolver, a reference that leaves the document is refused at
 * compile time - the same refusal as any other reference that does not
 * resolve, and for the same reason.
 *
 * There is one exception, and it is a set of nine fixed documents rather than
 * a hole in the rule. The 2020-12 meta-schemas - the root at
 * `https://json-schema.org/draft/2020-12/schema` and the eight under
 * `.../2020-12/meta/` - are embedded in this library, so a schema that says
 * "this instance is a valid schema" resolves with no resolver present. That
 * is not a convenience: the dialect describes itself, so the alternative is a
 * validator that opens a connection during a compile, to a URI it read out of
 * the document it was handed. These URIs also do not version - the reference
 * model rests on each naming one fixed document forever - so an embedded copy
 * cannot fall behind a newer one, which is why the Unicode tables this
 * library derives are treated the other way round.
 *
 * A resolver supplied here is still asked first and still wins, for those
 * URIs as for any other. Serving your own copy - a mirror, a stricter
 * variant - is a thing a caller may legitimately want to do, and a bundled
 * document that could not be overridden would be this library deciding it
 * knows better.
 *
 * The meta-schemas constrain `$id` and `$anchor` with `pattern`, so a `$ref`
 * that reaches them needs `regex` set as well. Without a provider the compile
 * is refused, because reporting a schema as valid on the strength of two
 * constraints that were never checked is worse than declining to answer.
 */
typedef struct {
  /**
   * User-defined, passed to each call.
   */
  void * ctx;

  /**
   * Return the schema document for `uri`, or NULL if there is none.
   *
   * `uri` is an absolute URI with no fragment, NUL-terminated, of `uri_len`
   * bytes. The returned value is borrowed and must stay alive until
   * gtext_json_schema_compile_with_options() returns; nothing is kept after
   * that, because everything a compiled schema needs has been copied out of
   * it by then.
   */
  const GTEXT_JSON_Value * (*get_fn)(
      void * ctx, const char * uri, size_t uri_len);
} GTEXT_JSON_Schema_Resolver;

/**
 * @brief What the `format` keyword does
 */
typedef enum {
  /**
   * `format` is an annotation and asserts nothing.
   *
   * This is the specification's default and the default here. An
   * implementation that refuses a schema for carrying `format`, as this one
   * did, is not conformant: 2020-12 says a validator MUST NOT assert on it
   * unless it has been asked to.
   */
  GTEXT_JSON_FORMAT_ANNOTATION = 0,

  /**
   * Assert every format this library can check.
   *
   * A format name in the 2020-12 vocabulary that it cannot check is refused
   * at compile time with GTEXT_JSON_E_SCHEMA_UNSUPPORTED, rather than
   * ignored - the caller asked for the constraint, and handing back a schema
   * that silently does not carry it is the failure the strict-keyword check
   * exists to prevent. A name outside the vocabulary - a vendor's own
   * `"format": "phone-number"` - is ignored, because the specification
   * requires that and because nothing was promised about it.
   *
   * Checked: date-time, date, time, duration (ghoti.io-chron's grammars),
   * email, idn-email, hostname, idn-hostname, ipv4, ipv6, uri,
   * uri-reference, iri, iri-reference, uuid, uri-template, json-pointer,
   * relative-json-pointer, and regex when a regular-expression provider was
   * supplied.
   *
   * Refused: regex when no provider was supplied, which is the only name in
   * the vocabulary this library cannot answer on its own.
   *
   * `hostname` and `idn-hostname` are IDNA2008 (RFC 5890 to 5893), including
   * decoding an `xn--` label and checking what it decodes to - 2020-12
   * section 7.3.3 defines `hostname` to include Punycode-produced names, so
   * the LDH rule alone is not the keyword.
   *
   * `idn-hostname` additionally runs UTS #46's mapping and normalisation step
   * first, nontransitional: fullwidth forms are folded to their ASCII
   * counterparts, ignorable characters such as a zero-width space are
   * removed, and the result is put into Normalization Form C, so that a name
   * spelled with a combining acute is the same name as one spelled with the
   * precomposed character. The four deviation characters - sharp s, final
   * sigma and the two zero-width joiners - are left alone, which is what
   * every current browser does.
   *
   * The two specifications stay layered rather than merged: UTS #46 says what
   * the name becomes, and RFC 5892 still says what is valid, because 2020-12
   * section 7.3.4.3 defines this format by RFC 5890. A plain `hostname` is
   * not mapped at all.
   */
  GTEXT_JSON_FORMAT_ASSERT
} GTEXT_JSON_Format_Policy;

/**
 * @brief Options controlling schema compilation
 */
typedef struct {
  /**
   * Accept schemas that use standard keywords this implementation does not
   * enforce, ignoring those keywords instead of refusing the schema.
   *
   * Default: false. Setting it restores the behavior of releases before the
   * strict check existed, in which a schema using `$ref`, `allOf`, `pattern`
   * or `additionalProperties` compiled successfully and then validated
   * instances those keywords should have rejected. Set it only when the
   * ignored keywords are known to be decorative.
   */
  bool allow_unsupported_keywords;

  /**
   * The regular-expression engine `pattern` and `patternProperties` are
   * enforced with, or NULL for none.
   *
   * With a provider, both keywords are compiled at schema-compile time and
   * checked at validation time. Without one, both are refused at compile time
   * the way any other unenforceable keyword is - unless
   * allow_unsupported_keywords is set, which ignores them along with the rest.
   *
   * The pointer is borrowed, and so is everything reachable from its `ctx`.
   * Both must outlive every schema compiled with them, because the compiled
   * patterns are released when the schema is freed.
   */
  const GTEXT_JSON_Regex_Provider * regex;

  /**
   * Whether `format` asserts anything. Default:
   * GTEXT_JSON_FORMAT_ANNOTATION, which is what the specification requires
   * of a validator that has not been asked otherwise.
   */
  GTEXT_JSON_Format_Policy format;

  /**
   * Where a `$ref` that leaves this document is fetched from, or NULL for
   * nowhere - in which case such a reference is refused.
   *
   * The pointer is borrowed, and so is everything reachable from its `ctx`.
   * Both need only outlive the compile call.
   */
  const GTEXT_JSON_Schema_Resolver * resolver;

  /**
   * The base URI the document is compiled against, or NULL for none.
   *
   * A schema retrieved from `https://example.com/s.json` has that as its
   * base whether or not it carries an `$id` saying so, and a relative `$ref`
   * inside it means something different without it. NUL-terminated.
   */
  const char * base_uri;

  /**
   * Which dialect a document that carries no `$schema` is written in, as the
   * `$schema` URI that would have named it, or NULL for 2020-12.
   *
   * `$schema` is optional, and a document that omits it does not thereby
   * become dialect-free: it was written against something, and the drafts
   * disagree about what keywords mean rather than only about which exist.
   * draft-07 says a schema object containing `$ref` *is* that reference and
   * its siblings are ignored; 2019-09 onward apply them. `additionalItems`
   * governs the tail of a positional `items` in draft-06 through 2019-09 and
   * does not exist in 2020-12. Reading one draft's document under another's
   * rules gives a wrong answer about the instance, not an unknown keyword.
   *
   * Guessing 2020-12 is the right default - it is the current draft - but it
   * is a guess, and a caller who knows better could not say so. This is how
   * they say so. A `$schema` inside the document always wins, and wins per
   * resource, so this only decides what an undeclared one means.
   *
   * The value is one of the dialect URIs this library reads; anything else is
   * refused at compile time rather than silently ignored. NUL-terminated and
   * borrowed, and need only outlive the compile call.
   */
  const char * default_dialect;

  /**
   * Where the compiled schema's memory comes from, or NULL for the C library.
   *
   * This covers the whole compiled object and everything it owns: the schema
   * structure, every compiled node, the `$ref` registry, the resource and
   * `$dynamicAnchor` tables, the base URI, the URI strings built while
   * resolving references, the clone of the schema document, and the
   * per-validation marks gtext_json_schema_validate() uses. The allocator is
   * recorded on the schema, so validating and freeing it need no options of
   * their own.
   *
   * Borrowed, and it must outlive the schema rather than the compile call -
   * which is why this is an option and not inherited from the document.
   * gtext_json_patch_apply() and gtext_json_clone() take the allocator of the
   * tree they are given, because what they produce lives in that tree and
   * dies with it. A compiled schema is a separate object with its own
   * lifetime: the header promises the document may be freed after compiling,
   * so a schema built from the document's allocator would outlive the reason
   * the caller chose it. The caller has to say, and this is where.
   *
   * gtext_json_schema_compile(), which takes no options, therefore uses the C
   * library.
   */
  const GTEXT_Allocator * allocator;
} GTEXT_JSON_Schema_Options;

/**
 * @brief Get the default schema compilation options
 *
 * @return Options with allow_unsupported_keywords set to false and no
 *   regular-expression provider.
 */
GTEXT_API GTEXT_JSON_Schema_Options gtext_json_schema_options_default(void);

/**
 * @brief Compile a JSON Schema document into a compiled schema
 *
 * Parses and validates a JSON Schema document, compiling it into an
 * internal representation for efficient validation. The schema document
 * must be a valid JSON object.
 *
 * The compiled schema is independent of the original schema document;
 * the document can be freed after compilation.
 *
 * @param schema_doc JSON value representing the schema document (must not be
 *   NULL; an object, or a boolean, which is a schema accepting everything or
 *   nothing)
 * @param err Error output structure (can be NULL if error details not needed)
 * @return Compiled schema on success, NULL on failure (check err for details)
 */
GTEXT_API GTEXT_JSON_Schema * gtext_json_schema_compile(
    const GTEXT_JSON_Value * schema_doc, GTEXT_JSON_Error * err);

/**
 * @brief Compile a JSON Schema document with explicit options
 *
 * Identical to gtext_json_schema_compile() except that the caller chooses
 * how unsupported standard keywords are treated.
 *
 * @param schema_doc JSON value representing the schema document (must not be
 *   NULL, must be GTEXT_JSON_OBJECT)
 * @param opts Compilation options, or NULL for the defaults
 * @param err Error output structure (can be NULL if error details not needed).
 *   When a schema is refused for using an unsupported keyword, the code is
 *   GTEXT_JSON_E_SCHEMA_UNSUPPORTED and context_snippet holds the keyword
 *   name. When a `pattern` or `patternProperties` regular expression fails to
 *   compile, the code is GTEXT_JSON_E_INVALID - a malformed pattern is a
 *   malformed schema, the same fault as `"properties": 3` - context_snippet
 *   holds the provider's own message, and `offset` the byte offset it
 *   reported within the pattern, or `(size_t)-1` if it reported none. Either
 *   way, free it with gtext_json_error_free().
 * @return Compiled schema on success, NULL on failure
 */
GTEXT_API GTEXT_JSON_Schema * gtext_json_schema_compile_with_options(
    const GTEXT_JSON_Value * schema_doc,
    const GTEXT_JSON_Schema_Options * opts, GTEXT_JSON_Error * err);

/**
 * @brief Free a compiled schema
 *
 * Releases all memory associated with a compiled schema. After calling
 * this function, the schema pointer is invalid and must not be used.
 *
 * @param schema Schema to free (can be NULL, in which case this is a no-op)
 */
GTEXT_API void gtext_json_schema_free(GTEXT_JSON_Schema * schema);

/**
 * @brief Validate a JSON value against a compiled schema
 *
 * Validates a JSON value against a compiled schema. Returns GTEXT_JSON_OK
 * if the value matches the schema, or GTEXT_JSON_E_SCHEMA if validation fails.
 *
 * Error details are provided in the err structure, including which schema
 * keyword failed and why.
 *
 * @param schema Compiled schema (must not be NULL)
 * @param instance JSON value to validate (must not be NULL)
 * @param err Error output structure (can be NULL if error details not needed)
 * @return GTEXT_JSON_OK if validation succeeds, GTEXT_JSON_E_SCHEMA if
 * validation fails
 */
GTEXT_API GTEXT_JSON_Status gtext_json_schema_validate(
    const GTEXT_JSON_Schema * schema, const GTEXT_JSON_Value * instance,
    GTEXT_JSON_Error * err);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_JSON_JSON_SCHEMA_H
