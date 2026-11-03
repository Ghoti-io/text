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
 * Allocator abstraction for the Ghoti.io Text library.
 *
 * This is cutil's @ref GCU_Allocator under a local name, the same arrangement
 * the compress, image and model libraries use. One definition across the suite
 * means an allocator written for any of them works with all of them, rather
 * than needing a near-identical copy per library.
 *
 * `text` declared its own copy for a while, on the reading that being
 * standalone meant no dependency on cutil. That was a misreading: a dependency
 * inside the suite is fine so long as the graph stays a DAG, and cutil is its
 * root. The copy is gone and this is a typedef, so a caller who was already
 * using `GTEXT_Allocator` needs no change.
 */

#ifndef GHOTI_IO_GTEXT_ALLOCATOR_H
#define GHOTI_IO_GTEXT_ALLOCATOR_H

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/text/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All four function pointers must be provided. Each receives the `ctx` pointer
 * from the struct as its first argument, so one implementation can serve many
 * independent pools.
 *
 * Two requirements beyond the C library equivalents, which callers rely on:
 * `calloc_fn` must treat overflow of `nitems * size` as an allocation failure
 * and return NULL rather than allocating a truncated block, and a zero-size
 * request should return a usable non-NULL pointer, so that NULL always means
 * failure.
 *
 * A NULL `GTEXT_Allocator *` anywhere in this library means "use
 * gtext_allocator_default()", so every function taking one may be called
 * without one.
 */
typedef GCU_Allocator GTEXT_Allocator;

/**
 * @brief Get the default, stdlib-backed allocator.
 *
 * The returned pointer is to a process-global constant and never needs to be
 * freed. Its `calloc_fn` returns NULL on multiplication overflow, and neither
 * it nor `malloc_fn` returns NULL for a zero-size request.
 *
 * @return A pointer to the default allocator.
 */
GTEXT_API const GTEXT_Allocator * gtext_allocator_default(void);

/**
 * @brief Allocate through an allocator, defaulting when none is supplied.
 *
 * @param allocator The allocator to use, or NULL for the default.
 * @param size The number of bytes requested.
 * @return The allocated memory, or NULL on failure.
 */
GTEXT_API void * gtext_allocator_malloc(
    const GTEXT_Allocator * allocator, size_t size);

/**
 * @brief Allocate zeroed memory through an allocator.
 *
 * @param allocator The allocator to use, or NULL for the default.
 * @param nitems The number of items to allocate.
 * @param size The size of each item.
 * @return The allocated memory, or NULL on failure (including overflow).
 */
GTEXT_API void * gtext_allocator_calloc(
    const GTEXT_Allocator * allocator, size_t nitems, size_t size);

/**
 * @brief Resize an allocation through an allocator.
 *
 * @param allocator The allocator to use, or NULL for the default.
 * @param ptr The allocation to resize, or NULL to allocate afresh.
 * @param size The new size in bytes.
 * @return The resized memory, or NULL on failure, in which case `ptr` is
 *   still valid.
 */
GTEXT_API void * gtext_allocator_realloc(
    const GTEXT_Allocator * allocator, void * ptr, size_t size);

/**
 * @brief Release an allocation through an allocator.
 *
 * @param allocator The allocator to use, or NULL for the default.
 * @param ptr The allocation to release. NULL is ignored.
 */
GTEXT_API void gtext_allocator_free(
    const GTEXT_Allocator * allocator, void * ptr);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ALLOCATOR_H
