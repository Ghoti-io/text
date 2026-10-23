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
 * What the math files share: the builder that makes (and eagerly simplifies)
 * the nodes of a calculation in an arena, the type algebra, and the numeric
 * functions that fold a node whose arguments are known.
 *
 * css_math.c reads the component values of a math function (an explicit stack
 * of frames, so nesting is a limit and not a recursion) and serializes and
 * resolves a tree; css_math_simplify.c holds the node constructors with
 * Values 4, 10.10.1's simplification and the folds of the functions.
 *
 * Reference: CSS Values and Units Module Level 4, Editor's Draft of 24 August
 * 2026, section 10, https://drafts.csswg.org/css-values-4/#calc-func.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_MATH_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_MATH_INTERNAL_H

#include <ghoti.io/text/css/css_math.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef GTEXT_CSS_Math_Node mnode;

/// A builder: the arena the nodes go in, the limits, the scratch stack and what went wrong.
typedef struct {
  css_arena * arena;
  GTEXT_Allocator alloc;       ///< Scratch (the stack of operands).
  size_t max_depth, max_nodes;
  size_t nodes, work;
  GTEXT_CSS_Status status;     ///< OK, or OOM / a limit.
  bool invalid;                ///< The function is not a valid math function.
  GTEXT_CSS_Span where;        ///< For a limit.
  bool saw_percent;            ///< A percentage token was read.
  const mnode ** stk;          ///< Scratch: operands, in regions per open frame.
  size_t nstk, capstk;
} mbuilder;

/// Set a status once (the first wins).
void mb_fail(mbuilder * mb, GTEXT_CSS_Status st);
/// A math function with these operands is not valid.
void mb_invalid(mbuilder * mb);

/// A zeroed node of `op`, counted against the limit (NULL, with the status, when it fails).
mnode * mb_node(mbuilder * mb, GTEXT_CSS_Math_Op op);
/// Room for `n` kids in the arena (NULL, with the status, on failure).
const mnode ** mb_kids(mbuilder * mb, size_t n);

/* ---- types ----------------------------------------------------------- */

bool css_math_type_add(const GTEXT_CSS_Math_Type * a, const GTEXT_CSS_Math_Type * b, GTEXT_CSS_Math_Type * out);
bool css_math_type_mul(const GTEXT_CSS_Math_Type * a, const GTEXT_CSS_Math_Type * b, GTEXT_CSS_Math_Type * out);
GTEXT_CSS_Math_Type css_math_type_invert(const GTEXT_CSS_Math_Type * a);
GTEXT_CSS_Math_Category css_math_type_category(const GTEXT_CSS_Math_Type * t);
bool css_math_type_is_number(const GTEXT_CSS_Math_Type * t);
/// The type of a unit (length for px ...), or the zero type.
GTEXT_CSS_Math_Type css_math_unit_type(GTEXT_CSS_Unit unit);
/// The canonical unit of a category (NONE for a number or a percentage).
GTEXT_CSS_Unit css_math_category_unit(GTEXT_CSS_Math_Category cat);

/* ---- leaves ---------------------------------------------------------- */

const mnode * mk_number(mbuilder * mb, double v);
/// A dimension; an absolute unit is converted to its canonical one.
const mnode * mk_dimension(mbuilder * mb, double v, GTEXT_CSS_Unit unit);
const mnode * mk_percent(mbuilder * mb, double v);
const mnode * mk_symbol(mbuilder * mb, const char * name, size_t len, const GTEXT_CSS_Math_Type * type);
const mnode * mk_unresolved(mbuilder * mb, const char * text, size_t len, const GTEXT_CSS_Math_Type * type);

/* ---- operators (each simplifies the node it makes, its operands being simplified) ---- */

const mnode * mk_sum(mbuilder * mb, const mnode * const * terms, size_t n);
const mnode * mk_product(mbuilder * mb, const mnode * const * factors, size_t n);
const mnode * mk_negate(mbuilder * mb, const mnode * x);
const mnode * mk_invert(mbuilder * mb, const mnode * x);
/// A function: `args` are its operands in order (a `none` of clamp() is NULL); `round` for ROUND (with `written`).
const mnode * mk_fn(mbuilder * mb, GTEXT_CSS_Math_Op op, const mnode * const * args, size_t n,
    GTEXT_CSS_Math_Round round, bool written);

/// Whether a node is a numeric leaf.
bool css_math_is_numeric(const mnode * n);

/* ---- the numeric functions (exposed for the tests of the instrument) ---- */

/// The exact remainder of x by y: the sign of x, the magnitude below |y|, no rounding (shift and subtract).
GTEXT_INTERNAL_API double css_math_fmod_exact(double x, double y);
/// The value of the unary/binary/n-ary function `op` over canonical doubles; false for an op it does not compute.
GTEXT_INTERNAL_API bool css_math_apply(GTEXT_CSS_Math_Op op, GTEXT_CSS_Math_Round round, const double * a, size_t n, double * out);

/* ---- reading the component values --------------------------------------- */

/// Whether a function name is a math function or a numeric function that stands in a calculation (sibling-index() ...).
bool css_math_accepts_function(const char * name, size_t length);

/// Build the calculation of `function` into `arena`. The status is OK with *valid false when it is not valid.
GTEXT_CSS_Status css_math_build(css_arena * arena, const GTEXT_Allocator * scratch, const GTEXT_CSS_Component_Value * function,
    const GTEXT_CSS_Math_Context * ctx, size_t max_depth, size_t max_nodes, GTEXT_CSS_Math ** out, bool * valid,
    GTEXT_CSS_Math_Result * result);

/// Serialize a calculation into a growable byte buffer; false on OOM or the limit (status says).
GTEXT_CSS_Status css_math_serialize_to(const GTEXT_CSS_Math * math, const GTEXT_Allocator * alloc, size_t max_bytes,
    char ** out, size_t * out_len);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_MATH_INTERNAL_H
