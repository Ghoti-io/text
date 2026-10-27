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
 * The custom properties of a computed style: a persistent map from a name to a custom property value, in the order of the names. Private.
 *
 * A style inherits the custom properties of its parent and adds its own, and a chain of n elements that each declare one has n of them at its end.
 * Copying the parent's table at every level would be quadratic in the depth; this is a persistent treap (immutable nodes, path copying, the priority a
 * hash of the name, keyed by the context so that no names make it deep), so setting a name costs O(log n) new nodes and shares the rest with the parent's map, and a style that declares nothing holds the
 * parent's root itself. A node is reference counted, atomically: styles are freed on any thread. Nothing here recurses; the treap's height is
 * logarithmic because the priorities are a hash of the names, and a path that is deeper than the stack (which no set of names makes) is
 * GTEXT_CSS_E_LIMIT_DEPTH.
 *
 * A name set to no value (`--x: initial`, or `unset` where the parent has none) is a node with no value, a tombstone, which hides the name without
 * removing a node: nothing is ever taken out of a persistent map here.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_CMAP_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_CMAP_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_custom.h>
#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct css_cmap css_cmap;

/// Another owner of a map (NULL is the empty map).
css_cmap * css_cmap_retain(css_cmap * map);

/// One owner fewer; the last frees the nodes that nobody else holds. NULL is ignored.
void css_cmap_release(css_cmap * map);

/// How many names have a value.
size_t css_cmap_count(const css_cmap * map);

/// A hash of the shape of the tree (every node's name, depth and priority, and whether it has a value) and, in `*height` (may be NULL), its height; 0 and
/// (size_t)-1 for a tree deeper than the stack. For tests: a treap's shape is a function of its names and priorities alone, whatever order they were set in.
uint64_t css_cmap_shape(const css_cmap * map, size_t * height);

/// The value of a name, or NULL where there is none (the name is absent or hidden).
const GTEXT_CSS_Custom_Value * css_cmap_find(const css_cmap * map, const char * name, size_t length);

/// How many names that have a value are before `name` in the order of the names (the place `name` would be at).
size_t css_cmap_rank(const css_cmap * map, const char * name, size_t length);

/// The `index`th name that has a value, in the order of the names (by bytes, then by length); false past the end.
bool css_cmap_at(const css_cmap * map, size_t index, const char ** name, size_t * length, const GTEXT_CSS_Custom_Value ** value);

/// A map that is `map` with `name` set to `value` (retained; NULL hides the name). `*out` is a new owner; `map` is not touched, and stays as it was
/// whatever happens. `seed` keys the priority of a new name: one seed for every map of a family (a context). GTEXT_CSS_OK, GTEXT_CSS_E_OOM or GTEXT_CSS_E_LIMIT_DEPTH.
/// `work` (may be NULL) is increased by the nodes read and the nodes made.
GTEXT_CSS_Status css_cmap_set(const GTEXT_Allocator * alloc, css_cmap * map, const char * name, size_t length, GTEXT_CSS_Custom_Value * value, uint64_t seed,
    css_cmap ** out, size_t * work);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_CMAP_H
