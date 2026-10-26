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
 * What the value model's files share: the private part of a value set, and the
 * name test for the arbitrary substitution functions.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_VALUE_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_VALUE_INTERNAL_H

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// A set of typed values and the memory they live in.
struct GTEXT_CSS_Value_Set {
  GTEXT_Allocator alloc;           ///< The set itself and its blocks come from this.
  css_arena arena;                 ///< Every value, string and array.
  const GTEXT_CSS_Value * root;    ///< The root value, or NULL.
  size_t count;                    ///< Values in all.
  GCU_Atomic_Int refs;             ///< Owners: 1 when made; gtext_css_value_set_free() releases one.
  GTEXT_CSS_Value_Set ** deps;     ///< Sets this one keeps alive because its values point into them (each retained once); NULL if none.
  size_t ndeps;                    ///< Their number.
  size_t depcap;                   ///< Allocated entries of `deps`.
};

/// A new, empty set with one owner. NULL when the allocator refused.
GTEXT_CSS_Value_Set * css_vset_new(const GTEXT_Allocator * alloc);

/// Make `set` keep `other` alive for as long as it lives (a value of `set` points into `other`). `other` is retained; the same set is recorded once.
/// False (nothing recorded) when memory ran out.
bool css_vset_depend(GTEXT_CSS_Value_Set * set, const GTEXT_CSS_Value_Set * other);

/// Whether a function name is var, env or attr (ASCII case-insensitive).
bool css_is_substitution_name(const GTEXT_CSS_String * name);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_VALUE_INTERNAL_H
