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
 * @file ini_value.h
 * @brief Turning a raw INI value into a string, a list, a number or a boolean.
 *
 * **This is a separate layer on purpose, and both reference implementations put
 * it in the same place.** `g_key_file_load_from_data()` accepts a document
 * containing `k=a\qb`; `g_key_file_get_value()` hands back `a\qb`; and only
 * `g_key_file_get_string()` fails, because `\q` is not in Desktop Entry §4's
 * escape set. `systemd.syntax(7)` is explicit about the same split: quoting
 * applies "for settings where quoting is allowed", which the grammar cannot
 * know because it depends on the setting.
 *
 * So a document with an undefined escape in it **parses**, and the error
 * surfaces here, where a caller asked a question that depends on it. The
 * practical consequence is that a validator and a reader disagree about such a
 * document and both are right - @ref format_ini records that neither reference
 * answers it, and that the conformance gate therefore excludes those documents
 * and counts them rather than scoring them.
 *
 * Numbers are read in the **C locale** regardless of `LC_NUMERIC`, because
 * Desktop Entry §4 defines `numeric` as "a valid floating point number as
 * recognized by the `%f` specifier for scanf in the C locale". A process with
 * `LC_NUMERIC=de_DE.UTF-8` must still read `1.5` as one and a half, and must
 * still refuse `1,5`.
 */

#ifndef GHOTI_IO_GTEXT_INI_INI_VALUE_H
#define GHOTI_IO_GTEXT_INI_INI_VALUE_H

#include <ghoti.io/text/ini/ini_dom.h>
#include <ghoti.io/text/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode a raw value's escapes.
 *
 * Applies exactly the set ::GTEXT_INI_Dialect::escapes names and refuses
 * anything else with ::GTEXT_INI_E_BAD_ESCAPE - including a trailing lone
 * backslash, which names no sequence at all. A dialect with no escapes copies
 * the bytes.
 *
 * `\;` is deliberately *not* accepted here even though Desktop Entry uses it,
 * because §4 defines it only inside a list: a bare `a\;b` is refused by
 * `g_key_file_get_string()` and accepted by `g_key_file_get_string_list()`, and
 * gtext_ini_value_list() is where it is handled.
 *
 * @param dialect The dialect whose escape set to apply. Must not be NULL.
 * @param raw The raw value.
 * @param raw_len Length of @p raw.
 * @param alloc Allocator for the result, or NULL for the default. The same one
 *   must be passed to gtext_ini_string_free().
 * @param out Receives a NUL-terminated buffer. Must not be NULL.
 * @param out_len Receives its length, not counting the terminator. May be NULL.
 *   Authoritative: a decoded value may contain a NUL if the raw bytes did.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_BAD_ESCAPE, ::GTEXT_INI_E_INVALID or
 *   ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_unescape(
    const GTEXT_INI_Dialect * dialect, const char * raw, size_t raw_len,
    const GTEXT_Allocator * alloc, char ** out, size_t * out_len);

/**
 * @brief Encode a string so that a reader of the same dialect returns it.
 *
 * The inverse of gtext_ini_unescape(), and the function a caller needs before
 * gtext_ini_group_set() if the value might contain a newline, a tab, a
 * backslash, or a leading space - a dialect with no quoting has no other way
 * to spell those.
 *
 * Reports ::GTEXT_INI_E_UNREPRESENTABLE when the dialect's escape set cannot
 * spell a byte that must be escaped, rather than emitting it raw. A dialect
 * with no escapes therefore refuses a value containing a newline, which is
 * correct: there is no such document.
 *
 * @param dialect The dialect. Must not be NULL.
 * @param text The bytes to encode.
 * @param text_len Length of @p text.
 * @param alloc Allocator for the result, or NULL for the default.
 * @param out Receives a NUL-terminated buffer. Must not be NULL.
 * @param out_len Receives its length. May be NULL.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_UNREPRESENTABLE,
 *   ::GTEXT_INI_E_INVALID or ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_escape(const GTEXT_INI_Dialect * dialect,
    const char * text, size_t text_len, const GTEXT_Allocator * alloc,
    char ** out, size_t * out_len);

/**
 * @brief Release a buffer from gtext_ini_unescape() or gtext_ini_escape().
 *
 * @param alloc The allocator that produced it, or NULL for the default. Must
 *   match.
 * @param text The buffer. NULL is ignored.
 */
GTEXT_API void gtext_ini_string_free(const GTEXT_Allocator * alloc,
    char * text);

/**
 * @brief A decoded list of strings. Opaque.
 */
typedef struct GTEXT_INI_List GTEXT_INI_List;

/**
 * @brief Split a raw value on the dialect's list separator and decode each
 *   item.
 *
 * Desktop Entry §4: items are separated by `;`, the value "may be optionally
 * terminated by a semicolon", `\;` is a literal semicolon, and **"trailing
 * empty strings must always be terminated with a semicolon"** - so `a;b;` is
 * two items and `a;b;;` is three, the last of them empty. That asymmetry is
 * the specification's and is why a trailing separator cannot simply be
 * discarded.
 *
 * @param dialect The dialect. Must not be NULL, and must have a
 *   ::GTEXT_INI_Dialect::list_separator.
 * @param raw The raw value.
 * @param raw_len Length of @p raw.
 * @param alloc Allocator, or NULL for the default.
 * @param out Receives the list. Release with gtext_ini_list_free().
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_BAD_ESCAPE, ::GTEXT_INI_E_INVALID (no
 *   separator in the dialect) or ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_value_list(
    const GTEXT_INI_Dialect * dialect, const char * raw, size_t raw_len,
    const GTEXT_Allocator * alloc, GTEXT_INI_List ** out);

/**
 * @brief How many items a list has.
 *
 * @param list The list.
 * @return The count, or 0 if @p list is NULL.
 */
GTEXT_API size_t gtext_ini_list_count(const GTEXT_INI_List * list);

/**
 * @brief An item of a list.
 *
 * @param list The list.
 * @param index 0-based.
 * @param len Receives the item's length. May be NULL.
 * @return The item, NUL-terminated, or NULL if out of range.
 */
GTEXT_API const char * gtext_ini_list_at(const GTEXT_INI_List * list,
    size_t index, size_t * len);

/**
 * @brief Release a list.
 *
 * @param list The list. NULL is ignored.
 */
GTEXT_API void gtext_ini_list_free(GTEXT_INI_List * list);

/**
 * @brief Split a raw value into **words**, systemd's spelling of a list.
 *
 * One pass over the raw value doing four things at once, which is how systemd's
 * `extract_first_word()` does them and why they cannot be separate steps: joining
 * a line continuation, removing quotes, decoding escapes, and splitting on
 * unquoted whitespace. Decoding first and splitting second would be wrong - a
 * `\"` would become a quote character indistinguishable from one the document
 * wrote.
 *
 * Measured against systemd 257, and three of the rules are not in
 * `systemd.syntax(7)`:
 *
 *   - **Quoting is a toggle, not a wrapper**: `x"y z"` is the one word `xy z`,
 *     and `a"b c"` is `ab c`. The manual says an opening quote may appear only at
 *     the start or after unquoted whitespace; systemd does not enforce that.
 *   - `"` and `'` both quote and each is **literal inside the other**.
 *   - **Escapes are decoded inside single quotes too**, unlike a shell.
 *
 * An empty quoted run produces an **empty word** (`"" x` is two words); a leading
 * or trailing whitespace run produces none. An unclosed quote is
 * ::GTEXT_INI_E_BAD_LINE, which is what systemd reports as "Invalid syntax" while
 * discarding the setting.
 *
 * The result is the same ::GTEXT_INI_List gtext_ini_value_list() returns, because
 * a caller asks it the same questions - only the spelling of the separator
 * differs, and for this dialect there is not one.
 *
 * @param dialect The dialect. Must not be NULL, and must have
 *   ::GTEXT_INI_Dialect::word_split.
 * @param raw The raw value, as the parser stored it.
 * @param raw_len Length of @p raw.
 * @param alloc Allocator, or NULL for the default.
 * @param out Receives the list. Release with gtext_ini_list_free().
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_BAD_LINE (an unclosed quote),
 *   ::GTEXT_INI_E_BAD_ESCAPE, ::GTEXT_INI_E_INVALID (the dialect does not split
 *   words) or ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_value_words(
    const GTEXT_INI_Dialect * dialect, const char * raw, size_t raw_len,
    const GTEXT_Allocator * alloc, GTEXT_INI_List ** out);

/**
 * @brief Read a raw value as a boolean, by the dialect's spellings.
 *
 * Desktop Entry §4: "Values of type boolean must either be the string `true`
 * or `false`." Nothing else is accepted there - not `1`, not `yes`, not `True` -
 * because the specification admits nothing else and a reader that guessed would be
 * reading a different dialect. systemd admits eight words, and
 * ::GTEXT_INI_Dialect::bool_style is which set applies.
 *
 * **The dialect parameter arrived with systemd**, and this function was the only
 * one in the value layer without one - gtext_ini_unescape(), gtext_ini_escape() and
 * gtext_ini_value_list() all take a dialect, because what a value *means* is the
 * dialect's business. That it did not was the anomaly, so the parameter was added
 * rather than a second function.
 *
 * @param dialect The dialect. Must not be NULL.
 * @param raw The raw value.
 * @param raw_len Length of @p raw.
 * @param out Receives the value. Must not be NULL.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_TYPE or ::GTEXT_INI_E_INVALID.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_value_bool(
    const GTEXT_INI_Dialect * dialect, const char * raw, size_t raw_len,
    bool * out);

/**
 * @brief Read a raw value as a signed 64-bit integer.
 *
 * Decimal only, with an optional leading `-` or `+`, and no surrounding
 * whitespace - the value's leading whitespace was already removed by the
 * parser, and a trailing run is part of the value under this dialect, so a
 * value with one is not an integer.
 *
 * @param raw The raw value.
 * @param raw_len Length of @p raw.
 * @param out Receives the value. Must not be NULL.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_TYPE, ::GTEXT_INI_E_RANGE or
 *   ::GTEXT_INI_E_INVALID.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_value_int(const char * raw,
    size_t raw_len, int64_t * out);

/**
 * @brief Read a raw value as a double, in the C locale.
 *
 * Desktop Entry §4 defines `numeric` by reference to `%f` in the C locale, so
 * this accepts what `strtod` accepts there and refuses `1,5` in every locale.
 * It also refuses `inf` and `nan`: `%f` reads them, and a desktop entry file
 * has no use for either, so they are ::GTEXT_INI_E_TYPE rather than silently
 * becoming a value no writer can spell back.
 *
 * @param raw The raw value.
 * @param raw_len Length of @p raw.
 * @param out Receives the value. Must not be NULL.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_TYPE, ::GTEXT_INI_E_RANGE or
 *   ::GTEXT_INI_E_INVALID.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_value_double(const char * raw,
    size_t raw_len, double * out);

/**
 * @enum GTEXT_INI_Interpolation
 * @brief Which reference syntax gtext_ini_value_interpolate() resolves.
 *
 * **Not a dialect field, and that placement is the decision rather than a
 * detail.** Interpolation is a pass over an already-assembled value: no
 * reference in a value changes how the document is tokenized, which lines are
 * continuations, or which entries exist. Putting it in ::GTEXT_INI_Dialect would
 * say the opposite, and would also make it a property of *the document* when it
 * is a property of the question a caller is asking.
 *
 * So every dialect here parses `k = 100%` and hands back `100%`, and a caller who
 * wants `configparser`'s reading asks for it. @ref format_ini records the
 * measurement behind that default: over the 479 real documents on this machine
 * that `configparser` reads, its own default ::GTEXT_INI_INTERPOLATION_BASIC
 * **refuses a value in 301 of them and changes a value in none**, and of the five
 * files containing `%(` or `${` not one is a reference this enum could resolve -
 * numpy's `npymath.ini` spells pkg-config's variables, which share the syntax and
 * mean something else. A reader that interpolated by default would read a third
 * of this machine's INI files worse.
 */
typedef enum GTEXT_INI_Interpolation {
  /**
   * No pass. A `%` or a `$` is an ordinary byte.
   *
   * The default everywhere in this module, and what `ConfigParser(interpolation=
   * None)` does.
   */
  GTEXT_INI_INTERPOLATION_NONE = 0,
  /**
   * `%(key)s`, with `%%` for a literal `%`. `configparser`'s `BasicInterpolation`.
   *
   * **A `%` that begins neither is ::GTEXT_INI_E_INTERPOLATION**, not a literal.
   * That refusal is the half of this style people forget: it makes
   * `BasicInterpolation` a validator and not only a substitution, and it is what
   * a bare `100%` runs into.
   *
   * The key is looked up in the group the value came from, then in
   * GTEXT_INI_Interpolate_Options::defaults, using the dialect's own key folding -
   * which for this module is ASCII. `configparser` folds with `str.lower()`, so a
   * reference to a key spelled with a non-ASCII capital resolves there and not
   * here; that divergence is @ref format_ini's `nonascii-upper-key` and the
   * `configparser` gate asserts it is still observable.
   */
  GTEXT_INI_INTERPOLATION_BASIC,
  /**
   * `${key}` and `${section:key}`, with `$$` for a literal `$`.
   * `configparser`'s `ExtendedInterpolation`, which is opt-in there too.
   *
   * `%` is an ordinary byte under this style and `$` is not, so the two styles do
   * not nest and are not orderable: `a%%b` is `a%%b` here and `a%b` under
   * ::GTEXT_INI_INTERPOLATION_BASIC. Measured against CPython, which is also why
   * the gate needs a run of its own for each.
   */
  GTEXT_INI_INTERPOLATION_EXTENDED
} GTEXT_INI_Interpolation;

/**
 * @brief How gtext_ini_value_interpolate() resolves a reference.
 *
 * Zero-initializing this struct gives ::GTEXT_INI_INTERPOLATION_NONE, no
 * defaults group, the reference's own depth limit and the default allocator -
 * so a caller who wants one field sets one field. Use
 * gtext_ini_interpolate_options_default() rather than writing the fields, so
 * that a field added later does not change what an existing caller means.
 */
typedef struct GTEXT_INI_Interpolate_Options {
  /** Which reference syntax to resolve. */
  GTEXT_INI_Interpolation style;
  /**
   * A group consulted when the value's own group does not have the key, or NULL.
   *
   * This is `configparser`'s `[DEFAULT]`, and it is a parameter because that
   * section is a **lookup policy over a parsed tree and not a rule of the
   * grammar** - the same reason the `configparser` oracle pins
   * `default_section` to a name no document can spell. A caller reproducing
   * Python's reading passes `gtext_ini_document_group(doc, "DEFAULT")` here; one
   * that wants sections to be independent passes NULL.
   *
   * Consulted second, never first, and only for a reference. It does not make
   * the key visible to gtext_ini_group_get().
   */
  const GTEXT_INI_Group * defaults;
  /**
   * How many references deep a chain may go before
   * ::GTEXT_INI_E_INTERPOLATION, or 0 for `configparser`'s limit of 10.
   *
   * A limit and not a cycle detector, which is the reference's choice and is
   * also the weaker claim: `a = %(b)s` and `b = %(a)s` is a cycle, and
   * `%(b)s` through eleven hops is not, and both stop here. A cap is what makes
   * the pass terminate on a tree a caller assembled rather than parsed.
   */
  unsigned max_depth;
  /**
   * Largest result to build, or 0 for a **bound proportional to the input** -
   * which is not the same thing as no bound.
   *
   * **The polarity is deliberately the opposite of
   * ::GTEXT_INI_Parse_Options::max_total_bytes, and the reason is that here there
   * is recursion to bound.** A parse is flat, so its count limits are policy and
   * 0 means "no limit"; interpolation *expands*, and ::max_depth caps how deep a
   * chain goes without capping how large it gets. `a = ${b}${b}` with `b`
   * referring to `c` the same way doubles per hop, so ten hops is a thousandfold
   * and a megabyte of input is a gigabyte of output. `configparser` has no bound
   * at all here; a C library that copied that would have a documented
   * amplification.
   *
   * The default is `max(65536, 16 * raw_len)`: proportional, so a large document
   * is not refused for being large, and bounded, so the amplification factor is
   * 16 rather than unbounded. Exceeding it is ::GTEXT_INI_E_LIMIT, which is
   * distinct from both interpolation refusals because it is a statement about
   * this call and not about the document.
   *
   * Pass `SIZE_MAX` for no bound, and get `configparser`'s own behaviour. One
   * sentinel is not being made to mean two things: 0 is the default and
   * `SIZE_MAX` is unlimited, and no value means both.
   */
  size_t max_output;
  /** Allocator for the result, or NULL for the default. The same one must be
   *  passed to gtext_ini_string_free(). */
  const GTEXT_Allocator * allocator;
} GTEXT_INI_Interpolate_Options;

/**
 * @brief The options gtext_ini_value_interpolate() uses when given NULL.
 *
 * @return ::GTEXT_INI_INTERPOLATION_NONE, no defaults, depth 10, the
 *   proportional output bound, the default allocator.
 */
GTEXT_API GTEXT_INI_Interpolate_Options
gtext_ini_interpolate_options_default(void);

/**
 * @brief Resolve the references in a raw value.
 *
 * Faithful to `configparser`'s `BasicInterpolation` and `ExtendedInterpolation`,
 * including the parts that are not substitution: a trigger byte that begins no
 * reference is ::GTEXT_INI_E_INTERPOLATION, a reference to a key that is not
 * there is ::GTEXT_INI_E_INTERPOLATION_MISSING, and a substituted value
 * containing a trigger byte is itself interpolated - under
 * ::GTEXT_INI_INTERPOLATION_EXTENDED **relative to the section it came from**,
 * which is why a `${other:k}` chain can walk the document.
 *
 * ::GTEXT_INI_INTERPOLATION_NONE copies the bytes, so a caller holding a style in
 * a variable needs no branch of its own.
 *
 * **gtext_ini_unescape() first, this second**, and that order is the reference's
 * rather than a preference. `read()` stores a value already *joined* -
 * `'\n'.join(val)` - and `get()` interpolates what it stored, so the text a
 * reference resolves against is the joined one. Pass gtext_ini_unescape()'s
 * output as @p raw for a value the parser produced; a value read straight out of
 * the tree still carries the terminators and the indentation its continuation
 * lines spanned, and interpolating that would substitute a different string on
 * every multi-line value. The same join is applied to whatever a reference
 * *resolves to*, so a caller cannot get the two halves out of step.
 *
 * Interpolation is nonetheless not the escape pass: `configparser` has no escape
 * set at all, so for that dialect gtext_ini_unescape() is exactly the join and
 * nothing more.
 *
 * **No *reference* has both escapes and interpolation, but this API composes them**,
 * because the style is a parameter and not a dialect field - so a caller may hand
 * this function a group from a Desktop Entry, git or systemd document, and those
 * three do have an escape set. What happens then is defined rather than accidental:
 * a value a reference **resolves to** is decoded by its own group's dialect, so
 * `Target=a\nb` substituted through `%(Target)s` arrives as a real newline, and a
 * referenced value holding an escape the set does not define is
 * ::GTEXT_INI_E_BAD_ESCAPE. That is consistent with @p raw only if the caller
 * followed the order above and passed gtext_ini_unescape()'s output; pass the tree's
 * raw bytes under one of those dialects and the two halves of the result are decoded
 * to different depths. No reference can settle the composition, so this is stated,
 * and IniInterpolation.ACrossDialectReferenceUsesItsOwnDialect asserts it.
 *
 * @param group The group @p raw came from, whose keys a reference resolves
 *   against. May be NULL, and then only GTEXT_INI_Interpolate_Options::defaults
 *   is consulted. The document reached through it is what a
 *   `${section:key}` walks.
 * @param options How to resolve. NULL means
 *   gtext_ini_interpolate_options_default(), which resolves nothing.
 * @param raw The raw value.
 * @param raw_len Length of @p raw. A NUL inside it is data, as everywhere here.
 * @param out Receives a NUL-terminated buffer. Must not be NULL. Release with
 *   gtext_ini_string_free().
 * @param out_len Receives its length, not counting the terminator. May be NULL.
 *   Authoritative: a resolved value may contain a NUL if a raw one did.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_INTERPOLATION,
 *   ::GTEXT_INI_E_INTERPOLATION_MISSING, ::GTEXT_INI_E_LIMIT
 *   (GTEXT_INI_Interpolate_Options::max_output), ::GTEXT_INI_E_BAD_ESCAPE (a
 *   value a reference resolved to is not decodable under the group's dialect,
 *   which `configparser` cannot produce and a dialect with an escape set could),
 *   ::GTEXT_INI_E_INVALID or ::GTEXT_INI_E_OOM.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_value_interpolate(
    const GTEXT_INI_Group * group,
    const GTEXT_INI_Interpolate_Options * options, const char * raw,
    size_t raw_len, char ** out, size_t * out_len);

/**
 * @brief Whether interpolating @p raw under @p style could return anything other
 *   than @p raw.
 *
 * **Deliberately "contains the trigger byte" and not "contains a well-formed
 * reference"**, because the case a caller most needs to hear about is the one a
 * stricter predicate would call clean: `100%` holds no reference, and it is
 * exactly what ::GTEXT_INI_INTERPOLATION_BASIC refuses. A detector that answered
 * false for it would be silent on 301 of this machine's 479 real documents while
 * gtext_ini_value_interpolate() refused them.
 *
 * So this is the predicate for "can I use the raw value as-is under this style",
 * and false is the load-bearing answer. It reads bytes only and asks the document
 * nothing, so it cannot distinguish a reference that resolves from one that does
 * not - gtext_ini_value_interpolate() is the only thing that can.
 *
 * @param style The style. ::GTEXT_INI_INTERPOLATION_NONE always yields false.
 * @param raw The raw value.
 * @param raw_len Length of @p raw.
 * @return True when @p raw contains `%` (basic) or `$` (extended).
 */
GTEXT_API bool gtext_ini_value_needs_interpolation(
    GTEXT_INI_Interpolation style, const char * raw, size_t raw_len);

/**
 * @brief Look a key up with Desktop Entry §5's locale fallback.
 *
 * The match order for an `LC_MESSAGES` of `lang_COUNTRY.ENCODING@MODIFIER`,
 * from §5 and implemented in this order: `lang_COUNTRY@MODIFIER`,
 * `lang_COUNTRY`, `lang@MODIFIER`, `lang`, then the unpostfixed key. The
 * encoding is ignored when matching. If `LC_MESSAGES` has no modifier, no key
 * with a modifier is matched; if it has no country, no key with a country is.
 *
 * @param group The group.
 * @param key The bare key, with no postfix.
 * @param locale The locale to match, in the form above, or NULL to take
 *   `LC_MESSAGES` from the environment. Pass `""` to ask for the unpostfixed
 *   key only.
 * @param len Receives the value's length. May be NULL.
 * @return The raw value of the best match, or NULL if not even the unpostfixed
 *   key is present.
 */
GTEXT_API const char * gtext_ini_group_get_locale(
    const GTEXT_INI_Group * group, const char * key, const char * locale,
    size_t * len);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_INI_INI_VALUE_H
