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
 * @file toml_json.h
 * @brief Converting between a TOML tree and a JSON one.
 *
 * The two data models are nearly the same shape and differ in four places,
 * which is what the options here are for. Neither conversion invents a value:
 * where one format cannot hold what the other has, a policy says whether to
 * refuse or what to put instead, and the default is the one that loses nothing
 * silently.
 *
 * | | TOML | JSON |
 * |---|---|---|
 * | no counterpart in JSON | the four date-time types | - |
 * | no counterpart in JSON | `inf`, `-inf`, `nan` | - |
 * | no counterpart in TOML | - | `null` |
 * | no counterpart in TOML | - | a number outside `int64_t` |
 *
 * Everything else crosses exactly: a table is an object, an array is an array,
 * and TOML's integer and float stay an integer and a float - JSON writes both
 * as numbers, so this is where the two are still distinguishable.
 *
 * **A round trip is an identity except at two of those places**, and both are
 * the format's doing rather than this code's:
 *
 *   - a date-time becomes a string, and a string stays a string, so
 *     `d = 1979-05-27` comes back as `d = "1979-05-27"`. Re-reading strings
 *     that look like dates is not on offer: a TOML string that happens to
 *     spell a date is a string, and a conversion that guessed would change a
 *     document's meaning to make its own round trip look better;
 *   - a float whose shortest spelling carries no point and no exponent - `1.0`,
 *     or `1e3` - is written `1` and `1000` by JSON, which has one number type,
 *     and comes back as a TOML *integer*. The information is gone at the JSON
 *     end, not here.
 *
 * `@ref format_toml` gives the measurement: the corpus mode that sends every
 * valid case through JSON and back computes that population from the suite's
 * own expectations rather than from a list kept here.
 */

#ifndef GHOTI_IO_GTEXT_TOML_TOML_JSON_H
#define GHOTI_IO_GTEXT_TOML_TOML_JSON_H

#include <ghoti.io/text/json/json_dom.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/toml/toml_core.h>
#include <ghoti.io/text/toml/toml_dom.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum GTEXT_TOML_JSON_Datetime_Policy
 * @brief What to do with a TOML date-time, which JSON has no type for.
 */
typedef enum {
  /**
   * Write it as a string, in exactly the spelling gtext_toml_write() would
   * have used - `chron`'s `gchron_write_toml()`, RFC 3339 where the value has
   * an offset. The default: the value survives in full and only its *type*
   * does not, which is the smaller loss and the one every JSON consumer of a
   * date already expects.
   */
  GTEXT_TOML_JSON_DATETIME_STRING = 0,
  /**
   * Refuse the document with GTEXT_TOML_E_UNREPRESENTABLE, for a caller who
   * needs to know rather than to guess afterwards which of their strings used
   * to be dates.
   */
  GTEXT_TOML_JSON_DATETIME_ERROR
} GTEXT_TOML_JSON_Datetime_Policy;

/**
 * @enum GTEXT_TOML_JSON_Nonfinite_Policy
 * @brief What to do with `inf`, `-inf` or `nan`, which JSON cannot write.
 *
 * The default here is to refuse, where a date-time's is to convert, and the
 * difference is not a mood: a date-time keeps its value as a string and a
 * non-finite float has no JSON spelling that keeps anything. Both of the
 * alternatives below lose the value, so a caller has to choose which loss they
 * want rather than being handed one.
 */
typedef enum {
  /** GTEXT_TOML_E_UNREPRESENTABLE. The default. */
  GTEXT_TOML_JSON_NONFINITE_ERROR = 0,
  /** The strings `"inf"`, `"-inf"` and `"nan"` - TOML's own spellings. */
  GTEXT_TOML_JSON_NONFINITE_STRING,
  /** `null`, which is what JavaScript's `JSON.stringify` does. */
  GTEXT_TOML_JSON_NONFINITE_NULL
} GTEXT_TOML_JSON_Nonfinite_Policy;

/**
 * @enum GTEXT_TOML_JSON_Null_Policy
 * @brief What to do with a JSON `null`, which TOML has no value for.
 *
 * TOML has no null and this is not an oversight in TOML: "a key with no value"
 * is spelled by leaving the key out.
 */
typedef enum {
  /** GTEXT_TOML_E_UNREPRESENTABLE. The default. */
  GTEXT_TOML_JSON_NULL_ERROR = 0,
  /**
   * Leave the member out, which is how a TOML document says the same thing.
   *
   * Only a member of an object can be left out. A `null` *inside an array* is
   * refused whatever this says, because dropping it would shorten the array and
   * every later index would move - a different document rather than the same one
   * with a gap.
   */
  GTEXT_TOML_JSON_NULL_SKIP
} GTEXT_TOML_JSON_Null_Policy;

/**
 * @struct GTEXT_TOML_To_JSON_Options
 * @brief What gtext_toml_to_json() does at the two places JSON is narrower.
 *
 * **There is no allocator here, and that is a consequence rather than an
 * omission.** The JSON DOM's constructors take none - `gtext_json_new_object()`
 * has no parameters - so every node this builds comes from the JSON module's
 * own arenas, and gtext_json_free() releases them. The one allocation this
 * conversion makes of its own is the walk's frame stack, from the default
 * allocator; an option covering only that would invite a caller to think it
 * covered the output.
 */
typedef struct {
  /** What to do with a date-time. Default GTEXT_TOML_JSON_DATETIME_STRING. */
  GTEXT_TOML_JSON_Datetime_Policy datetime;
  /** What to do with `inf` or `nan`. Default
   *  GTEXT_TOML_JSON_NONFINITE_ERROR. */
  GTEXT_TOML_JSON_Nonfinite_Policy nonfinite;
  /**
   * Maximum nesting to convert, or 0 for no limit. Default 256.
   *
   * This walk is on the heap, like every other walk in this module, so the
   * limit is not there to protect *this* code. It is there because
   * gtext_json_free() releases the tree it hands back **recursively**: a
   * converted document deeper than the JSON module's teardown can manage would
   * be a tree this library built and cannot release. Refusing it here is the
   * difference between an error a caller can act on and a crash in another
   * module's free.
   */
  size_t max_depth;
} GTEXT_TOML_To_JSON_Options;

/**
 * @struct GTEXT_TOML_From_JSON_Options
 * @brief What gtext_json_to_toml() does at the places TOML is narrower.
 */
typedef struct {
  /**
   * The allocator every node of the TOML tree comes from, or NULL for
   * gtext_allocator_default(). Unlike the other direction this one has
   * somewhere to go: the TOML constructors each take an allocator, and a tree
   * is freed through the one its root carries.
   */
  const GTEXT_Allocator * allocator;
  /** What to do with a JSON `null`. Default GTEXT_TOML_JSON_NULL_ERROR. */
  GTEXT_TOML_JSON_Null_Policy null_values;
  /** Maximum nesting to convert, or 0 for no limit. Default 256, as a parse's
   *  is; the TOML walks are all on the heap, so 0 costs memory and not a
   *  crash. */
  size_t max_depth;
} GTEXT_TOML_From_JSON_Options;

/**
 * @brief The defaults: a date-time becomes a string, a non-finite float is
 *   refused, depth 256.
 */
GTEXT_API GTEXT_TOML_To_JSON_Options gtext_toml_to_json_options_default(void);

/** @brief The defaults: the default allocator, `null` refused, depth 256. */
GTEXT_API GTEXT_TOML_From_JSON_Options gtext_toml_from_json_options_default(
    void);

/**
 * @brief Convert a TOML tree to a JSON one.
 *
 * The JSON value is the caller's and is released with gtext_json_free().
 * Nothing of the TOML tree is borrowed: every string is copied, so @p root may
 * be freed immediately afterwards.
 *
 * A TOML string that is not valid UTF-8 - which only a caller's own
 * gtext_toml_new_string() can produce, never a parse - is carried across as it
 * is, and refused by whichever writer is asked to write it. The check lives at
 * the point of output in both modules rather than being repeated here.
 *
 * @param root The document, or any value: a scalar converts to a scalar, since
 *   JSON has no rule that a document must be an object.
 * @param opts Options, or NULL for gtext_toml_to_json_options_default().
 * @param out_json Receives the value; set to NULL on failure. Must not be NULL.
 * @param err Receives the failure, or is zeroed. May be NULL. Its `line` and
 *   `col` are 0: there is no input text here, and a position invented from a
 *   tree walk would name nothing a caller could look at.
 * @return GTEXT_TOML_OK, GTEXT_TOML_E_INVALID for a NULL argument,
 *   GTEXT_TOML_E_DEPTH, GTEXT_TOML_E_OOM, GTEXT_TOML_E_DATETIME for a
 *   date-time `chron` will not spell, or GTEXT_TOML_E_UNREPRESENTABLE under the
 *   refusing policies.
 */
GTEXT_API GTEXT_TOML_Status gtext_toml_to_json(const GTEXT_TOML_Value * root,
    const GTEXT_TOML_To_JSON_Options * opts, GTEXT_JSON_Value ** out_json,
    GTEXT_TOML_Error * err);

/**
 * @brief Convert a JSON value to a TOML tree.
 *
 * The tree is the caller's and is released with gtext_toml_free().
 *
 * **A number is its lexeme**, for its type and for its value both: a lexeme
 * holding `.`, `e` or `E` is a float and anything else an integer, which is the
 * same question TOML asks of the same text. So `1` is an integer and `1.0` and
 * `1e3` are floats, and a JSON document written by something that spells whole
 * floats without a point produces TOML integers - correctly, because that is all
 * the text says.
 *
 * Not the int64 or double a JSON value happens to carry, which is a different
 * question and answers wrongly twice: a number this library built from a double
 * carries no int64 at all, and a range test on the double accepts
 * `-9223372036854775809`, since that literal is exactly -2^63 once converted.
 * The digits say what the digits say. A number parsed without
 * GTEXT_JSON_Parse_Options::preserve_number_lexeme has no lexeme and is
 * GTEXT_TOML_E_INVALID rather than a guess.
 *
 * @param json The value. Must be an object: v1.0.0 says "a TOML document is a
 *   table", so an array or a scalar at the top is
 *   GTEXT_TOML_E_UNREPRESENTABLE - a perfectly good JSON value with no TOML
 *   document to be.
 * @param opts Options, or NULL for gtext_toml_from_json_options_default().
 * @param out_root Receives the tree; set to NULL on failure. Must not be NULL.
 * @param err Receives the failure, or is zeroed. May be NULL.
 * @return GTEXT_TOML_OK, GTEXT_TOML_E_INVALID for a NULL argument or a number
 *   with no lexeme, GTEXT_TOML_E_DEPTH, GTEXT_TOML_E_OOM, GTEXT_TOML_E_RANGE
 *   for an integer outside `int64_t`, or GTEXT_TOML_E_UNREPRESENTABLE for a
 *   `null` under the refusing policy, a `null` in an array, or a root that is
 *   not an object.
 */
GTEXT_API GTEXT_TOML_Status gtext_json_to_toml(const GTEXT_JSON_Value * json,
    const GTEXT_TOML_From_JSON_Options * opts, GTEXT_TOML_Value ** out_root,
    GTEXT_TOML_Error * err);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_TOML_TOML_JSON_H
