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
 * @file macros.h
 * @brief Cross-compiler macros and utilities for the Ghoti.io Text library
 *
 * This header provides cross-compiler macros for common functionality
 * such as marking unused parameters, deprecated functions, and utility
 * macros for array operations.
 */

#ifndef GHOTI_IO_GTEXT_MACROS_H
#define GHOTI_IO_GTEXT_MACROS_H

// Every public header reaches this one, directly or through a module header,
// so the symbol renames land before any declaration they apply to.
#include <ghoti.io/text/namespace.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Typedef prototypes for forward declarations
 *
 * These forward declarations allow headers to reference types before
 * their full definitions are available.
 */
typedef struct GTEXT_JSON_Value GTEXT_JSON_Value;

/**
 * @brief Cross-compiler macro for marking a function parameter as unused
 *
 * Use this macro to suppress compiler warnings about unused parameters.
 * Works with GCC, Clang, and MSVC.
 *
 * @param X The parameter name to mark as unused
 *
 * Example:
 * @code
 * void my_function(int GTEXT_MAYBE_UNUSED(param)) {
 *     // param is intentionally unused
 * }
 * @endcode
 */
#if defined(__GNUC__) || defined(__clang__)
#define GTEXT_MAYBE_UNUSED(X) __attribute__((unused)) X

#elif defined(_MSC_VER)
#define GTEXT_MAYBE_UNUSED(X) (void)(X)

#else
#define GTEXT_MAYBE_UNUSED(X) X

#endif

/**
 * @brief Cross-compiler macro for marking a function as deprecated
 *
 * Use this macro to mark functions that are deprecated and will be
 * removed in a future version. Works with GCC, Clang, and MSVC.
 *
 * Example:
 * @code
 * GTEXT_DEPRECATED
 * void old_function(void);
 * @endcode
 */
#if defined(__GNUC__) || defined(__clang__)
#define GTEXT_DEPRECATED __attribute__((deprecated))

#elif defined(_MSC_VER)
#define GTEXT_DEPRECATED __declspec(deprecated)

#else
#define GTEXT_DEPRECATED

#endif

/**
 * @brief API export macro for cross-platform library symbols
 *
 * Use this macro to mark functions that should be exported from the
 * shared library. Automatically handles Windows DLL export/import
 * and Unix symbol visibility.
 *
 * On Windows, code that links the static archive rather than the DLL must
 * define GTEXT_STATIC: dllimport makes the compiler reference __imp_ thunks,
 * which only a DLL's import library provides. The test suite links the
 * archive, so the Makefile defines it there.
 *
 * Example:
 * @code
 * GTEXT_API void public_function(void);
 * @endcode
 */
#ifdef __cplusplus
#define GTEXT_EXTERN extern "C"
#else
#define GTEXT_EXTERN
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef GTEXT_BUILD
#define GTEXT_API GTEXT_EXTERN __declspec(dllexport)
#elif defined(GTEXT_STATIC)
#define GTEXT_API GTEXT_EXTERN
#else
#define GTEXT_API GTEXT_EXTERN __declspec(dllimport)
#endif
#else
#define GTEXT_API GTEXT_EXTERN __attribute__((visibility("default")))
#endif

/**
 * @brief Marks an exported *variable* as part of the public API.
 *
 * Same visibility as GTEXT_API, but without the `extern "C"`.  A variable
 * declaration cannot carry a redundant linkage specification - `extern "C"
 * extern int x;` is ill-formed in C++ - so a declaration that needs both
 * `extern` and export uses this and sits inside the header's `extern "C"`
 * block like every other declaration.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef GTEXT_BUILD
#define GTEXT_API_DATA __declspec(dllexport)
#elif defined(GTEXT_STATIC)
#define GTEXT_API_DATA
#else
#define GTEXT_API_DATA __declspec(dllimport)
#endif
#else
#define GTEXT_API_DATA __attribute__((visibility("default")))
#endif

/**
 * @brief Internal API export macro for testing
 *
 * This macro is used to export internal functions that are needed for testing
 * but should not be part of the public API. These functions are only exported
 * when GTEXT_TEST_BUILD is defined during library compilation.
 *
 * Example:
 * @code
 * GTEXT_INTERNAL_API void internal_function(void);
 * @endcode
 */
#ifdef GTEXT_TEST_BUILD
#define GTEXT_INTERNAL_API GTEXT_API
#else
#define GTEXT_INTERNAL_API
#endif

/**
 * @brief Compile-time array size helper
 *
 * Calculates the number of elements in a statically-allocated array.
 * This is a compile-time constant and safe to use in constant expressions.
 *
 * @param a The array (not a pointer)
 * @return The number of elements in the array
 *
 * Example:
 * @code
 * int arr[10];
 * size_t count = GTEXT_ARRAY_SIZE(arr);  // count = 10
 * @endcode
 */
#ifndef GTEXT_ARRAY_SIZE
#define GTEXT_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/**
 * @brief Bit manipulation macro
 *
 * Creates a bitmask with a single bit set at the specified position.
 *
 * @param x The bit position (0-based)
 * @return A bitmask with bit x set
 *
 * Example:
 * @code
 * unsigned int flags = GTEXT_BIT(3);  // flags = 0x08
 * @endcode
 */
#ifndef GTEXT_BIT
#define GTEXT_BIT(x) (1u << (x))
#endif

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_MACROS_H
