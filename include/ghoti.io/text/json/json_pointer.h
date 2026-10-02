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
 * JSON Pointer (RFC 6901) evaluation on JSON DOM.
 *
 * This header provides functions for evaluating JSON Pointer strings against
 * JSON DOM trees. JSON Pointers allow accessing nested values using a
 * path-like syntax (e.g., "/a/0/b").
 */

#ifndef GHOTI_IO_GTEXT_JSON_JSON_POINTER_H
#define GHOTI_IO_GTEXT_JSON_JSON_POINTER_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/macros.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get a value from a JSON DOM using a JSON Pointer
 *
 * Evaluates a JSON Pointer string against a JSON DOM tree and returns
 * a pointer to the referenced value. The returned pointer is valid for
 * the lifetime of the DOM tree.
 *
 * JSON Pointer format (RFC 6901):
 * - Empty string ("") refers to the root value
 * - "/" followed by reference tokens separated by "/"
 * - Reference tokens can contain escape sequences:
 *   - `~0` represents `~`
 *   - `~1` represents `/`
 * - Array indices are numeric strings (e.g., "0", "1")
 * - Object keys are reference tokens
 *
 * Examples:
 * - "" -> root value
 * - "/a" -> value at key "a" in root object
 * - "/0" -> first element of root array
 * - "/a/0/b" -> value at key "b" in first element of array at key "a"
 *
 * @param root Root JSON value to evaluate pointer against (must not be NULL)
 * @param ptr JSON Pointer string (must not be NULL)
 * @param len Length of pointer string in bytes
 * @return Pointer to the referenced value, or NULL if:
 *   - root is NULL
 *   - ptr is NULL
 *   - Pointer string is invalid
 *   - Referenced path does not exist
 *   - Array index is out of bounds
 *   - Object key is not found
 */
GTEXT_API const GTEXT_JSON_Value * gtext_json_pointer_get(
    const GTEXT_JSON_Value * root, const char * ptr, size_t len);

/**
 * @brief Get a mutable value from a JSON DOM using a JSON Pointer
 *
 * Same as gtext_json_pointer_get(), but returns a mutable pointer that
 * allows modification of the referenced value. The returned pointer is
 * valid for the lifetime of the DOM tree.
 *
 * @param root Root JSON value to evaluate pointer against (must not be NULL)
 * @param ptr JSON Pointer string (must not be NULL)
 * @param len Length of pointer string in bytes
 * @return Mutable pointer to the referenced value, or NULL on error
 *   (see gtext_json_pointer_get() for error conditions)
 */
GTEXT_API GTEXT_JSON_Value * gtext_json_pointer_get_mut(
    GTEXT_JSON_Value * root, const char * ptr, size_t len);

/**
 * @brief gtext_json_pointer_get() with a caller-supplied allocator
 *
 * Evaluating a pointer allocates one transient buffer per token - the decoded
 * form, since `~0` and `~1` mean a token is not always a span of the input - and
 * it is released before the next token is read. @p alloc is where that comes
 * from, so a caller holding an arena is not sent to the C library for it.
 *
 * **A separate entry point rather than a field, because neither
 * gtext_json_pointer_get() nor gtext_json_pointer_get_mut() takes options.**
 * The same shape gtext_csv_new_table_with_allocator() takes, and for the same
 * reason: there is nowhere in the existing signature to name one. The original
 * two delegate here with NULL, so what they do is unchanged.
 *
 * Nothing the pointer walk allocates outlives the call, so no free function
 * here needs the allocator back.
 *
 * @param root Root JSON value to evaluate pointer against (must not be NULL)
 * @param ptr JSON Pointer string (must not be NULL)
 * @param len Length of pointer string in bytes
 * @param alloc Allocator for the transient token buffer, or NULL for the
 *   default
 * @return Pointer to the referenced value, or NULL - see
 *   gtext_json_pointer_get() for the conditions
 */
GTEXT_API const GTEXT_JSON_Value * gtext_json_pointer_get_with_allocator(
    const GTEXT_JSON_Value * root, const char * ptr, size_t len,
    const GTEXT_Allocator * alloc);

/**
 * @brief gtext_json_pointer_get_mut() with a caller-supplied allocator
 *
 * See gtext_json_pointer_get_with_allocator().
 *
 * @param root Root JSON value to evaluate pointer against (must not be NULL)
 * @param ptr JSON Pointer string (must not be NULL)
 * @param len Length of pointer string in bytes
 * @param alloc Allocator for the transient token buffer, or NULL for the
 *   default
 * @return Mutable pointer to the referenced value, or NULL on error
 */
GTEXT_API GTEXT_JSON_Value * gtext_json_pointer_get_mut_with_allocator(
    GTEXT_JSON_Value * root, const char * ptr, size_t len,
    const GTEXT_Allocator * alloc);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_JSON_JSON_POINTER_H
