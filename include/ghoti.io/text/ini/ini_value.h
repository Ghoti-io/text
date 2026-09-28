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
 * @brief Read a raw value as a boolean.
 *
 * Desktop Entry §4: "Values of type boolean must either be the string `true`
 * or `false`." Nothing else is accepted - not `1`, not `yes`, not `True` -
 * because the specification admits nothing else and a reader that guessed
 * would be reading a different dialect. systemd's wider set (`1 yes true on`)
 * belongs to the systemd dialect, which is not implemented yet.
 *
 * @param raw The raw value.
 * @param raw_len Length of @p raw.
 * @param out Receives the value. Must not be NULL.
 * @return ::GTEXT_INI_OK, ::GTEXT_INI_E_TYPE or ::GTEXT_INI_E_INVALID.
 */
GTEXT_API GTEXT_INI_Status gtext_ini_value_bool(const char * raw,
    size_t raw_len, bool * out);

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
