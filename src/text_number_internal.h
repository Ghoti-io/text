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
 * Locale-independent number conversion, internal to the library.
 *
 * printf and strtod both read LC_NUMERIC, and in a good many locales its
 * decimal separator is a comma. Every format here has to convert numbers
 * without caring what locale the calling program happens to be in: the
 * separator belongs to the format, not to the user's language settings.
 * Three places had gone wrong on their own for want of somewhere shared to
 * put this, and each failed differently.
 *
 *   - A YAML "a: 0.1" came back as the *string* "0.1" rather than a float,
 *     because strtod stopped at a "." the locale did not recognise and the
 *     resolver takes a partial parse to mean "not a number".
 *   - gtext_json_new_number_double(0.1) wrote its lexeme with a bare
 *     snprintf, so the document it went into read "0,1" - not JSON at all.
 *   - A parsed JSON number quietly ended up with no double value, because
 *     the same partial parse left the has-double flag clear.
 *
 * The JSON writer already had the printf half of the answer, twice, behind
 * a portability #ifdef. It lives here now and everything uses it, on the
 * same reasoning as text_file_io_internal.h: keeping it in one place means
 * a fix to it reaches all three formats rather than one.
 *
 * These take a value and a style rather than a printf format string. That is
 * not decoration. The implementation works by repairing the one byte a
 * conversion can spell differently in another locale, and "the output is a
 * number and nothing else" is what makes that repair sound - a format string
 * with literal text in it would break the assumption silently. Taking the
 * value directly means the assumption cannot be violated by a caller.
 *
 * Nothing behind this header calls setlocale, uselocale, newlocale or
 * localeconv; see text_number.c for why the two-armed version that did was
 * the wrong shape for a library that ships to Windows as well as POSIX.
 */

#ifndef GHOTI_IO_GTEXT_TEXT_NUMBER_INTERNAL_H
#define GHOTI_IO_GTEXT_TEXT_NUMBER_INTERNAL_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How a double should be spelled.
 *
 * The three printf conversions this library ever wanted, named so that the
 * format string stays on this side of the call.
 */
typedef enum {
  GTEXT_NUMBER_GENERAL,    /**< Significant digits, shortest of %e / %f. */
  GTEXT_NUMBER_FIXED,      /**< Digits after the point. */
  GTEXT_NUMBER_SCIENTIFIC, /**< Exponential. */
} GTEXT_Number_Style;

/**
 * @brief Format a double with "." as the decimal separator, in any locale.
 *
 * Same return contract as snprintf: the length the output would have had, or
 * negative on failure. A return of @p buf_size or more means the result was
 * truncated and @p buf holds nothing worth reading.
 *
 * @param buf Destination buffer (must not be NULL)
 * @param buf_size Size of @p buf in bytes (must not be 0)
 * @param value The value to spell
 * @param style Which printf conversion to use
 * @param precision Precision for that conversion; negative is treated as 0
 * @return Bytes the result needed, or negative on failure
 */
GTEXT_INTERNAL_API int gtext_number_format_double(
    char * buf, size_t buf_size, double value,
    GTEXT_Number_Style style, int precision);

/**
 * @brief Format a signed 64-bit integer.
 *
 * Present for symmetry and to keep every number conversion in the library on
 * one path. Integer conversion has no locale-dependent element, so this is
 * snprintf with a bounds check.
 *
 * @param buf Destination buffer (must not be NULL)
 * @param buf_size Size of @p buf in bytes (must not be 0)
 * @param value The value to spell
 * @return Bytes the result needed, or negative on failure
 */
GTEXT_INTERNAL_API int gtext_number_format_i64(
    char * buf, size_t buf_size, int64_t value);

/**
 * @brief Format an unsigned 64-bit integer.
 *
 * @param buf Destination buffer (must not be NULL)
 * @param buf_size Size of @p buf in bytes (must not be 0)
 * @param value The value to spell
 * @return Bytes the result needed, or negative on failure
 */
GTEXT_INTERNAL_API int gtext_number_format_u64(
    char * buf, size_t buf_size, uint64_t value);

/**
 * @brief How many bytes strtod() would consume from @p s in the C locale.
 *
 * Exposed because it is the load-bearing half of gtext_number_strtod(): it
 * is what lets a parse stop where the document says the number stops, rather
 * than where the user's locale thinks it does. Handed "1,5" in a comma
 * locale, strtod answers 1.5; this answers 1, and the parse is cut there.
 *
 * @param s String to measure (must not be NULL)
 * @return Length of the longest prefix strtod would convert, or 0 for none
 */
GTEXT_INTERNAL_API size_t gtext_number_c_extent(const char * s);

/**
 * @brief strtod, reading "." as the decimal separator in any locale.
 *
 * Same contract as strtod, including setting errno on range errors, so a
 * caller can go on checking endptr and errno as it did before. @p end is set
 * relative to @p s, never into any temporary this function may have made.
 *
 * ## Why this takes an allocator
 *
 * In a locale whose separator is not "." the token has to be respelled before
 * strtod will read it, and a token has no bound on how long it may be written
 * - `0.000...1` is a valid spelling of a small double - so the respelling
 * buffer cannot always be a stack buffer. It was a bare malloc() until it
 * took this parameter, which made it the one piece of memory a caller who
 * supplied an allocator could not see: `make check-allocator-callees`
 * reported it as a recorded gap, reached from seven call sites.
 *
 * @p alloc may be NULL, which means gtext_allocator_default(), and that is
 * the right thing to pass from a public entry point that takes no allocator
 * of its own - gtext_ini_value_double() is one. It is not the right thing to
 * pass from inside a parse or a write that has the caller's allocator in
 * hand, and `make check-allocators` cannot tell the two apart, so the choice
 * is the caller's to make deliberately.
 *
 * Nothing is allocated at all in a "." locale, nor for a token short enough
 * for the internal stack buffer, so most callers never reach @p alloc.
 *
 * @param alloc Allocator for the respelling buffer, or NULL for the default
 * @param s String to convert (must not be NULL)
 * @param end Set to the first unconverted character, or NULL if not wanted
 * @return The converted value, or 0 on failure
 */
GTEXT_INTERNAL_API double gtext_number_strtod(
    const GTEXT_Allocator * alloc, const char * s, char ** end);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GTEXT_TEXT_NUMBER_INTERNAL_H */
