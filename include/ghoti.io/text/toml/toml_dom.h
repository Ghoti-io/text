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
 * @file toml_dom.h
 * @brief Parsing a TOML document, reading the tree that comes back, and
 *   building one from nothing.
 */

#ifndef GHOTI_IO_GTEXT_TOML_TOML_DOM_H
#define GHOTI_IO_GTEXT_TOML_TOML_DOM_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/toml/toml_core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TOML's four date-time types are chron values, as YAML's !!timestamp is. A
 * public include, so a consumer that reads a date-time out of a document needs
 * no second header to do anything with it. */
#include <ghoti.io/chron/chron.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_TOML_Type
 * @brief What a value is.
 *
 * Seven, where the specification lists ten: the four date-time types share
 * @ref GTEXT_TOML_DATETIME and are told apart by the `kind` field of the
 * GCHRON_TomlValue that gtext_toml_value_datetime() hands back.
 *
 * That is deliberate. Four enumerators here would be a second spelling of
 * GCHRON_TomlKind, and two spellings of one axis have to be kept in step by
 * whoever edits either - the kind is one field away from every caller who
 * needs it, and it is `chron`'s own vocabulary rather than a copy of it.
 */
typedef enum {
  GTEXT_TOML_TABLE,    ///< A table: the document root, a `[header]`, a dotted
                       ///< key's parent, or an inline `{ }`.
  GTEXT_TOML_ARRAY,    ///< An array, `[ ]` or the elements of `[[header]]`.
  GTEXT_TOML_STRING,   ///< Any of the four string forms; which form it was
                       ///< written in is not retained.
  GTEXT_TOML_INTEGER,  ///< int64_t.
  GTEXT_TOML_FLOAT,    ///< double, including the infinities and NaN.
  GTEXT_TOML_BOOLEAN,  ///< `true` or `false`, which are the only spellings.
  GTEXT_TOML_DATETIME  ///< One of the four; ask the GCHRON_TomlValue which.
} GTEXT_TOML_Type;

/**
 * @brief A node of a parsed document.
 *
 * Opaque. The whole tree belongs to the root that gtext_toml_parse() returned
 * and is released by one gtext_toml_free() of it; no node is freed on its own,
 * and no pointer into the tree outlives that call.
 */
typedef struct GTEXT_TOML_Value GTEXT_TOML_Value;

/**
 * @brief Parse a TOML document.
 *
 * The document is a table, so a successful parse always returns a
 * @ref GTEXT_TOML_TABLE, and an empty input returns an empty one rather than
 * failing: v1.0.0 has no notion of an absent root.
 *
 * `bytes` need not be NUL-terminated and is not modified. Nothing in the
 * returned tree points into it, so it may be released as soon as this
 * returns - TOML's strings need unescaping and its keys need joining, so there
 * is no in-situ mode to offer.
 *
 * @param bytes The document, UTF-8. A leading byte order mark is refused with
 *   GTEXT_TOML_E_BAD_TOKEN rather than skipped. **That is a choice, not a
 *   requirement**: v1.0.0 and the 1.1.0 draft say nothing about a byte order
 *   mark, and toml-test has no case for a leading one - only for one further
 *   in, which every version refuses. It is refused here because the pinned
 *   reference refuses it (CPython's `tomllib`, measured, not assumed), so a
 *   document this parser accepts is one that reference accepts. Recorded among
 *   the choices in @ref format_toml, where the specification's silence is what
 *   makes it a choice worth naming.
 * @param len Bytes of input.
 * @param opts Options, or NULL for gtext_toml_parse_options_default().
 * @param err Receives the failure and its position. May be NULL. On success it
 *   is zeroed, so one error struct can be reused across parses.
 * @return The root table, or NULL on failure.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_parse(const char * bytes, size_t len,
    const GTEXT_TOML_Parse_Options * opts, GTEXT_TOML_Error * err);

/**
 * @brief Parse a TOML file.
 *
 * Read incrementally rather than by seeking to the end, so a pipe, a FIFO or
 * /dev/stdin works as well as a regular file. `max_total_bytes` is applied
 * while reading.
 *
 * @param path The file to read.
 * @param opts Options, or NULL for the defaults.
 * @param err Receives the failure and its position. May be NULL.
 * @return The root table, or NULL on failure.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_parse_file(const char * path,
    const GTEXT_TOML_Parse_Options * opts, GTEXT_TOML_Error * err);

/**
 * @brief Release a parsed document.
 *
 * Walks the tree on the heap rather than on the C stack, so a document nested
 * as deeply as its parse allowed can always be freed. NULL is ignored.
 *
 * @param root The value gtext_toml_parse() returned.
 */
GTEXT_API void gtext_toml_free(GTEXT_TOML_Value * root);

/**
 * @brief What kind of value this is.
 *
 * @param value The value. NULL returns GTEXT_TOML_TABLE, which is why callers
 *   should test the pointer rather than the type.
 * @return The type.
 */
GTEXT_API GTEXT_TOML_Type gtext_toml_value_type(
    const GTEXT_TOML_Value * value);

/**
 * @brief A string value's bytes.
 *
 * The bytes are the *decoded* string: escapes resolved, a multi-line string's
 * leading newline and line-ending backslashes applied. Always NUL-terminated
 * for convenience, and `len` is authoritative because a TOML string may
 * contain a NUL (`\u0000` is a valid escape).
 *
 * @param value The value.
 * @param len Receives the length in bytes. May be NULL.
 * @return The bytes, or NULL if @p value is NULL or is not a string.
 */
GTEXT_API const char * gtext_toml_value_string(
    const GTEXT_TOML_Value * value, size_t * len);

/**
 * @brief An integer value.
 *
 * @param value The value.
 * @param out Receives the integer.
 * @return false if @p value is NULL, is not an integer, or @p out is NULL.
 */
GTEXT_API bool gtext_toml_value_integer(
    const GTEXT_TOML_Value * value, int64_t * out);

/**
 * @brief A float value.
 *
 * `inf`, `-inf`, `+inf`, `nan`, `-nan` and `+nan` are floats and arrive here
 * as the IEEE 754 values they name. TOML does not say which NaN, and neither
 * does this.
 *
 * @param value The value.
 * @param out Receives the double.
 * @return false if @p value is NULL, is not a float, or @p out is NULL.
 */
GTEXT_API bool gtext_toml_value_float(
    const GTEXT_TOML_Value * value, double * out);

/**
 * @brief A boolean value.
 *
 * @param value The value.
 * @param out Receives the boolean.
 * @return false if @p value is NULL, is not a boolean, or @p out is NULL.
 */
GTEXT_API bool gtext_toml_value_boolean(
    const GTEXT_TOML_Value * value, bool * out);

/**
 * @brief A date-time value, as `chron` read it.
 *
 * `out->kind` says which of the four it is, which is the distinction TOML
 * makes and this module's type enum does not.
 *
 * @param value The value.
 * @param out Receives the chron value.
 * @return false if @p value is NULL, is not a date-time, or @p out is NULL.
 */
GTEXT_API bool gtext_toml_value_datetime(
    const GTEXT_TOML_Value * value, GCHRON_TomlValue * out);

/**
 * @brief How many elements an array has.
 *
 * @param value The value.
 * @return The count, or 0 if @p value is NULL or is not an array.
 */
GTEXT_API size_t gtext_toml_array_size(const GTEXT_TOML_Value * value);

/**
 * @brief One element of an array.
 *
 * @param value The array.
 * @param index 0-based.
 * @return The element, or NULL if @p value is not an array or @p index is out
 *   of range.
 */
GTEXT_API const GTEXT_TOML_Value * gtext_toml_array_get(
    const GTEXT_TOML_Value * value, size_t index);

/**
 * @brief How many keys a table has, directly.
 *
 * Direct keys only: a table reached through a dotted key or a `[a.b]` header
 * is one entry of its parent, not two.
 *
 * @param value The table.
 * @return The count, or 0 if @p value is NULL or is not a table.
 */
GTEXT_API size_t gtext_toml_table_size(const GTEXT_TOML_Value * value);

/**
 * @brief The key at a position, in the order the document defined them.
 *
 * Definition order is retained because it is what a writer needs to round-trip
 * a document a human arranged, and because there is no other order a reader
 * could rely on. A key is the *decoded* string, so `a."b"`, `a.b` and `a.'b'`
 * all name the same entry here.
 *
 * @param value The table.
 * @param index 0-based.
 * @param len Receives the key's length in bytes. May be NULL.
 * @return The key, or NULL if @p value is not a table or @p index is out of
 *   range.
 */
GTEXT_API const char * gtext_toml_table_key_at(const GTEXT_TOML_Value * value,
    size_t index, size_t * len);

/**
 * @brief The value at a position, matching gtext_toml_table_key_at().
 *
 * @param value The table.
 * @param index 0-based.
 * @return The value, or NULL if @p value is not a table or @p index is out of
 *   range.
 */
GTEXT_API const GTEXT_TOML_Value * gtext_toml_table_value_at(
    const GTEXT_TOML_Value * value, size_t index);

/**
 * @brief Look a key up in a table.
 *
 * @param value The table.
 * @param key The decoded key. Not assumed to be NUL-terminated.
 * @param key_len Bytes of @p key.
 * @return The value, or NULL if @p value is not a table or the key is absent.
 */
GTEXT_API const GTEXT_TOML_Value * gtext_toml_table_get(
    const GTEXT_TOML_Value * value, const char * key, size_t key_len);

/*==========================================================================*
 * Building a document
 *
 * Enough to construct any TOML document from nothing, which is what the
 * writer needs to be measurable: toml-test's encoder direction hands a
 * harness the tagged JSON of a document and asks for TOML back, so without
 * these the write side could only ever be scored on trees this library had
 * parsed itself - a round trip, which proves the two halves agree with each
 * other rather than with TOML.
 *
 * Every constructor takes the allocator explicitly, because a node carries
 * the allocator it came from and one tree must not mix two. Passing a
 * different allocator to a constructor than the container's is refused by
 * gtext_toml_table_set() and gtext_toml_array_append() rather than quietly
 * producing a tree gtext_toml_free() would free through the wrong one.
 *==========================================================================*/

/**
 * @brief A new empty table.
 *
 * Counts as a table a `[header]` defined, so
 * GTEXT_TOML_TABLE_STYLE_AS_READ writes it as a header;
 * gtext_toml_value_set_inline() says otherwise.
 *
 * @param alloc The allocator, or NULL for gtext_allocator_default().
 * @return The table, or NULL on allocation failure.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_new_table(
    const GTEXT_Allocator * alloc);

/**
 * @brief A new empty array.
 *
 * Counts as an array written `[ ]`, so GTEXT_TOML_TABLE_STYLE_AS_READ writes
 * it inline even when every element is a table; gtext_toml_value_set_inline()
 * says otherwise.
 *
 * @param alloc The allocator, or NULL for gtext_allocator_default().
 * @return The array, or NULL on allocation failure.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_new_array(
    const GTEXT_Allocator * alloc);

/**
 * @brief A new string value, copying @p bytes.
 *
 * The bytes are the decoded string - what a reader would hand back, not what a
 * document contains. Escaping is the writer's job, and it will refuse bytes
 * that are not valid UTF-8 rather than write a document it cannot read back.
 *
 * @param alloc The allocator, or NULL for gtext_allocator_default().
 * @param bytes The string. May be NULL only when @p len is 0.
 * @param len Bytes of @p bytes. A NUL among them is data, not a terminator.
 * @return The value, or NULL on allocation failure or a NULL @p bytes with a
 *   non-zero @p len.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_new_string(
    const GTEXT_Allocator * alloc, const char * bytes, size_t len);

/**
 * @brief A new integer value.
 *
 * @param alloc The allocator, or NULL for gtext_allocator_default().
 * @param value The integer.
 * @return The value, or NULL on allocation failure.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_new_integer(
    const GTEXT_Allocator * alloc, int64_t value);

/**
 * @brief A new float value.
 *
 * The infinities and NaN are accepted: TOML spells all three, and a writer
 * that refused them would be narrower than the format.
 *
 * @param alloc The allocator, or NULL for gtext_allocator_default().
 * @param value The double.
 * @return The value, or NULL on allocation failure.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_new_float(
    const GTEXT_Allocator * alloc, double value);

/**
 * @brief A new boolean value.
 *
 * @param alloc The allocator, or NULL for gtext_allocator_default().
 * @param value The boolean.
 * @return The value, or NULL on allocation failure.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_new_boolean(
    const GTEXT_Allocator * alloc, bool value);

/**
 * @brief A new date-time value.
 *
 * The value is copied as it stands. Whether it is one TOML can spell is the
 * writer's question, not this one's, so an impossible date is reported when
 * the document is written rather than here - there is nothing useful a
 * constructor could say about it that gtext_toml_write() cannot say with a
 * position.
 *
 * @param alloc The allocator, or NULL for gtext_allocator_default().
 * @param value The chron value, whose `kind` says which of the four it is.
 * @return The value, or NULL on allocation failure or a NULL @p value.
 */
GTEXT_API GTEXT_TOML_Value * gtext_toml_new_datetime(
    const GTEXT_Allocator * alloc, const GCHRON_TomlValue * value);

/**
 * @brief Add a key to a table, taking ownership of @p value on success.
 *
 * On any failure the caller still owns @p value and must free it; on success
 * it belongs to @p table and is released by the one gtext_toml_free() of the
 * root.
 *
 * @param table The table.
 * @param key The decoded key. May be empty, which TOML permits as `""`, and
 *   may contain a NUL.
 * @param key_len Bytes of @p key.
 * @param value What to store.
 * @return
 *   - GTEXT_TOML_OK.
 *   - GTEXT_TOML_E_INVALID if @p table is not a table, or a pointer is NULL.
 *   - GTEXT_TOML_E_DUPKEY if the key is already there. Not a replacement:
 *     TOML says defining a key twice is invalid, and a builder that silently
 *     replaced would let a caller write a document that differs from what
 *     they described without anything saying so.
 *   - GTEXT_TOML_E_STATE if @p value is already in a tree, is @p table
 *     itself, or is an ancestor of it. Those three are the ways to make a
 *     shape that cannot be written or freed - the first is a double free, the
 *     other two a cycle - and they are refused here because no test could see
 *     them afterwards.
 *   - GTEXT_TOML_E_OOM.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_table_set(GTEXT_TOML_Value * table,
    const char * key, size_t key_len, GTEXT_TOML_Value * value);

/**
 * @brief Append to an array, taking ownership of @p value on success.
 *
 * TOML 1.0.0 arrays are heterogeneous (Array: "values of differing types may
 * be mixed"), so nothing here checks that the element matches its neighbours.
 *
 * @param array The array.
 * @param value What to append.
 * @return As gtext_toml_table_set(), without GTEXT_TOML_E_DUPKEY.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_array_append(
    GTEXT_TOML_Value * array, GTEXT_TOML_Value * value);

/**
 * @brief Say whether a container should be written inline.
 *
 * Sets what GTEXT_TOML_TABLE_STYLE_AS_READ reproduces: for a table, `{ }`
 * against `[header]`; for an array, `[ ]` against `[[header]]`. It changes
 * nothing about the value, and the other two table styles ignore it.
 *
 * A table read from a `{ }` in the document is already inline, so this is
 * mostly for a tree that was built rather than parsed.
 *
 * @param value A table or an array.
 * @param inline_style true for the inline spelling.
 * @return GTEXT_TOML_OK, or GTEXT_TOML_E_INVALID for NULL, or
 *   GTEXT_TOML_E_STATE for a scalar - a scalar has one spelling and a caller
 *   asking to change it has mistaken this for something else.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_value_set_inline(
    GTEXT_TOML_Value * value, bool inline_style);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_TOML_TOML_DOM_H
