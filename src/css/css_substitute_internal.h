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
 * What the substitution engine (css_substitute.c), the custom property values (css_custom.c) and the computed-value driver (css_compute.c) share. Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_SUBSTITUTE_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_SUBSTITUTE_INTERNAL_H

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/macros.h>

#include "css_compute_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the value of a custom property (css_custom.c) ---------------------------------------------------- */

struct GTEXT_CSS_Custom_Value {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  css_rope * rope;          ///< Never NULL: the empty value is a rope of no bytes.
};

/// A value around a rope (retained). NULL when memory ran out.
GTEXT_CSS_Custom_Value * css_cval_new(const GTEXT_Allocator * alloc, css_rope * rope);

/// The rope of a value (borrowed).
css_rope * css_cval_rope(const GTEXT_CSS_Custom_Value * value);

/// A rope of this text: its tokens counted and its ends classified by reading it as component values. `*out` is NULL when the text could not be read (a limit of the
/// parser). GTEXT_CSS_OK, GTEXT_CSS_E_OOM, or the limit.
GTEXT_CSS_Status css_rope_from_text(const GTEXT_Allocator * alloc, const char * text, size_t length, bool tainted, css_rope ** out);

/// The computed value of a registered custom property's initial value (it is computationally independent, so no element is needed): `*out` is a new value, or
/// NULL when the registration has no initial value. `st` is a state to compute in (a context being made, with no style).
GTEXT_CSS_Status css_ereg_initial(css_state * st, const css_creg * creg, GTEXT_CSS_Custom_Value ** out);

/// The effective registration of a name in a context, or NULL; `*index` receives its place in the context's table.
const css_ereg * css_ctx_find_reg(const GTEXT_CSS_Compute_Context * cc, const char * name, size_t length, size_t * index);

/// The compiled form of a syntax string for `attr( ... type(<syntax>) )`: from the context's cache, or compiled now. `*temporary` is set when the caller owns the
/// result (release it with css_compiled_syntax_free()) because the cache is full. GTEXT_CSS_E_GRAMMAR: not a syntax string.
GTEXT_CSS_Status css_ctx_syntax(const GTEXT_CSS_Compute_Context * cc, const char * text, size_t length, const css_compiled_syntax ** out, bool * temporary);

/// Make the registered custom properties of a context from the registry's and the rules' (the caller's win), computing their initial values. Part of making a context.
GTEXT_CSS_Status css_ctx_make_regs(GTEXT_CSS_Compute_Context * cc, css_state * st);
void css_ctx_free_regs(GTEXT_CSS_Compute_Context * cc);

/// A rope of the canonical text of a list of component values (white space collapsed, comments gone), for a value of a syntax that is only identifiers.
GTEXT_CSS_Status css_rope_from_list(const GTEXT_Allocator * alloc, const GTEXT_CSS_Component_Value * values, size_t count, bool tainted, css_rope ** out);

/// The rope of the text of a registered value that was computed by type (sRGB colors written as a computed color is).
GTEXT_CSS_Status css_registered_rope(css_state * st, const GTEXT_Allocator * alloc, const GTEXT_CSS_Value * root, bool tainted, css_rope ** out);

/// The registered values of a style: an array shared by every style that has the same ones (a style with a parent that changes none of them holds the
/// parent's), copied when a style declares one. Memory is a reference for a style that changes nothing, and not an array the length of the registrations.
typedef struct css_regvals css_regvals;
void css_regvals_release(css_regvals * r);

/// A style's registered values start as: the parent's where the property inherits, else the initial value (shared where that is what the parent has).
GTEXT_CSS_Status css_style_init_regvals(GTEXT_CSS_Computed_Style * style, const GTEXT_CSS_Computed_Style * parent);

/// The registered value `index` of a style, or NULL (the guaranteed-invalid value).
const GTEXT_CSS_Custom_Value * css_style_regval(const GTEXT_CSS_Computed_Style * style, size_t index);

/// Set the registered value `index` of a style (the value is retained), copying the shared array first if another style holds it.
GTEXT_CSS_Status css_style_set_regval(GTEXT_CSS_Computed_Style * style, size_t index, const GTEXT_CSS_Custom_Value * value);

/* ---- substitution (css_substitute.c) -------------------------------------------------------------------- */

typedef struct css_sub css_sub;

/// The substitution state of one element's computation: its declared custom properties are indexed. `*out` is NULL when there is nothing to substitute.
GTEXT_CSS_Status css_sub_new(css_state * st, css_sub ** out);
void css_sub_free(css_sub * sub);

/// Resolve every custom property the element declares, and put the values in the style (its map and its registered values). A cycle makes the properties in it
/// guaranteed-invalid.
GTEXT_CSS_Status css_sub_customs(css_sub * sub);

/// Replace the functions in a pending value (`prop` names the property or shorthand it is the value of, for the keys of random values). `*out` is a new rope,
/// or NULL for the guaranteed-invalid value.
GTEXT_CSS_Status css_sub_substitute(css_sub * sub, const char * prop, size_t prop_length, const GTEXT_CSS_Value_Set * pending, css_rope ** out);

/// The arguments of a value that is `cycle( <whole-value># )` and nothing else (CSS Values 5, 8.4), each as text. `*is_cycle` is false for any other value. `*invalid` is set when
/// the value is one that "declarations containing such constructs are invalid": an empty argument, or a nested `cycle()`, an `attr()` or a `calc()`. Release `*args`
/// with css_cycle_args_free().
typedef struct {
  char * text;
  size_t length;
} css_cycle_arg;

GTEXT_CSS_Status css_sub_cycle(css_sub * sub, const GTEXT_CSS_Value_Set * pending, bool * is_cycle, bool * invalid, css_cycle_arg ** args, size_t * nargs);
void css_cycle_args_free(css_sub * sub, css_cycle_arg * args, size_t nargs);

/// css_sub_substitute() for a text.
GTEXT_CSS_Status css_sub_substitute_text(css_sub * sub, const char * prop, size_t prop_length, const char * text, size_t length, css_rope ** out);

/// Write a rope as text for parsing (with the ranges that came from attr()); release `*text` and `*taint` with the allocator of the context.
GTEXT_CSS_Status css_sub_flatten(css_sub * sub, const css_rope * rope, char ** text, size_t * length, css_range ** taint, size_t * ntaint);

/// The counters of a finished computation.
void css_sub_report(const css_sub * sub, GTEXT_CSS_Compute_Result * result);

/// The random base value of a key written as text (what a `random()` calculation keeps of it; empty is `auto`), for the property being computed. False when the key
/// does not parse. Counts the call as one more random function of the property.
bool css_st_random_base(css_state * st, const char * key, size_t key_length, double * base);

/// min + R * (max - min), or the stepped value of Values 5, 9.3 (`has_step`).
double css_random_pick(double min, double max, double step, bool has_step, double R);

/// The number `sibling-index()` or `sibling-count()` stands for. GTEXT_CSS_E_UNSUPPORTED naming the callback when the tree has none.
GTEXT_CSS_Status css_st_sibling(css_state * st, bool index, double * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_SUBSTITUTE_INTERNAL_H
