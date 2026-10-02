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
 * @brief A set of DOM nodes, keyed on the pointer.
 *
 * Three walks need one, for the same reason and with the same question: the
 * resolver's alias repointing and its merge cycle check ask "have I been to
 * this node", and the writer asks "have I already written this node's anchor".
 * It was written once as a static inside yaml_resolve.c; the third caller is
 * in another translation unit, and two copies of a hash table is two places
 * for a growth bug to live.
 */

#include "yaml_internal.h"

/* Knuth's multiplicative constant for 64 bits.  The high bits of the product
   are the index, which mixes the low bits of the address - the varying ones,
   since the keys are addresses out of one arena - into the slot. */
#define GTEXT_YAML_NODE_SET_MULTIPLIER 0x9E3779B97F4A7C15ull

static size_t node_set_slot(
	const GTEXT_YAML_Node_Set *set,
	const GTEXT_YAML_Node *n
) {
	uint64_t h = (uint64_t)(uintptr_t)n * GTEXT_YAML_NODE_SET_MULTIPLIER;
	size_t i = (size_t)(h >> 32) & (set->capacity - 1);
	while (set->slots[i] && set->slots[i] != n) {
		i = (i + 1) & (set->capacity - 1);
	}
	return i;
}

static bool node_set_grow(GTEXT_YAML_Node_Set *set) {
	size_t new_capacity = set->capacity == 0 ? 64 : set->capacity * 2;
	const GTEXT_YAML_Node **slots = (const GTEXT_YAML_Node **)
		gtext_allocator_calloc(set->alloc, new_capacity, sizeof(*slots));
	if (!slots) return false;
	const GTEXT_YAML_Node **old_slots = set->slots;
	size_t old_capacity = set->capacity;
	set->slots = slots;
	set->capacity = new_capacity;
	for (size_t i = 0; i < old_capacity; i++) {
		if (old_slots[i]) {
			set->slots[node_set_slot(set, old_slots[i])] = old_slots[i];
		}
	}
	gtext_allocator_free(set->alloc, old_slots);
	return true;
}

GTEXT_INTERNAL_API void gtext_yaml_node_set_init(
	GTEXT_YAML_Node_Set *set,
	const GTEXT_Allocator *alloc
) {
	if (!set) return;
	set->slots = NULL;
	set->capacity = 0;
	set->count = 0;
	set->alloc = alloc;
}

GTEXT_INTERNAL_API bool gtext_yaml_node_set_add(
	GTEXT_YAML_Node_Set *set,
	const GTEXT_YAML_Node *n,
	bool *oom
) {
	/* Open addressing with no deletion, kept under three quarters full: that
	   bounds a probe run without a load-factor calculation at every insert. */
	if (set->count * 4 >= set->capacity * 3) {
		if (!node_set_grow(set)) {
			if (oom) *oom = true;
			return false;
		}
	}
	size_t i = node_set_slot(set, n);
	if (set->slots[i] == n) return false;
	set->slots[i] = n;
	set->count++;
	return true;
}

GTEXT_INTERNAL_API bool gtext_yaml_node_set_has(
	const GTEXT_YAML_Node_Set *set,
	const GTEXT_YAML_Node *n
) {
	if (!set || set->capacity == 0) return false;
	return set->slots[node_set_slot(set, n)] == n;
}

GTEXT_INTERNAL_API void gtext_yaml_node_set_free(GTEXT_YAML_Node_Set *set) {
	if (!set) return;
	gtext_allocator_free(set->alloc, set->slots);
	set->slots = NULL;
	set->capacity = 0;
	set->count = 0;
}
