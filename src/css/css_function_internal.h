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
 * What the function-family files share: the table entry that holds the grammar
 * of one type of the drafts (Value Definition Syntax text, as the draft writes it),
 * and the family of each function name.
 *
 * css_image.c: images and gradients (Images 3 and 4). css_transform.c: transform
 * functions (Transforms 1 and 2). css_easing.c: easing functions (Easing 1 and 2).
 * css_shape.c: basic shapes (Shapes 1) and the geometry boxes (Masking 1).
 * css_substitution.c: var(), env(), attr(), counters and the Values 5 functions.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_FUNCTION_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_FUNCTION_INTERNAL_H

#include <ghoti.io/text/css/css_function.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// One type of a draft: `name` as a resolver is asked for it (`image`, `linear-gradient()`), the grammar text, and where the draft is.
typedef struct {
  const char * name;
  GTEXT_CSS_Ref_Kind kind;
  const char * grammar;
} css_fn_type;

const css_fn_type * css_image_types(size_t * count);
const css_fn_type * css_transform_types(size_t * count);
const css_fn_type * css_easing_types(size_t * count);
const css_fn_type * css_shape_types(size_t * count);
const css_fn_type * css_substitution_types(size_t * count);

/// ASCII case-insensitive equality of a name with a lower-case literal.
bool css_fn_name_is(const GTEXT_CSS_String * s, const char * lower);

/// The leaves of the arguments of a function value, in order, with the commas between them: a NULL entry is a comma (from a
/// comma-separated list or a literal `,`). A nested function that is not a math function is a leaf, not descended into.
typedef struct {
  const GTEXT_CSS_Value ** v;
  size_t n, cap;
} css_leafvec;

/// Collect the leaves of `fn`'s children (a function value) into `out`. False on OOM (out is then freed).
bool css_fn_leaves(const GTEXT_Allocator * alloc, const GTEXT_CSS_Value * fn, css_leafvec * out);
void css_fn_leaves_free(const GTEXT_Allocator * alloc, css_leafvec * out);

/// What a leaf is as a number: a number or integer (0), a percentage (1), a length (2), an angle in degrees (3), a time in seconds (4), a resolution in dppx (5),
/// anything else (-1); a math function that came down to one value is that value. A relative length is not -1 but its own number (kind 2).
int css_fn_leaf_number(const GTEXT_CSS_Value * v, double * value);

/// The keyword a leaf is, lower case, or NULL.
bool css_fn_leaf_is_keyword(const GTEXT_CSS_Value * v, const char * lower);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_FUNCTION_INTERNAL_H
