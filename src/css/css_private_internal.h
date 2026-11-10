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
 * Private custom properties (CSS Functions and Mixins 1, "Private Custom Properties: the @private rule"), as the sheet compiler reads them (css_sheet.c). Private.
 *
 * A style rule that holds `@private` rules has a set of *private property names*. Within the rule and everything nested in it a custom property of such a name is a different property from
 * the custom property of the same name, and "Two private properties with the same name are considered the same property if and only if they have the same nearest style rule ancestor that
 * has their name in its private property names". The draft's own account of the effect is "roughly equivalent to" the rule written with a unique name for the property ("--f7bd60b7"), and
 * that is how it is built: the compiler gives each (style rule, name) a hidden custom property name, writes it where the rule declares the property and where the rule's declarations
 * refer to it (`var()`, and the other ways to name a custom property), and everything after the compiler (the cascade, inheritance, registration, substitution) handles a custom
 * property. The hidden names are not exposed (gtext_css_computed_custom_count() and the rest of the computed style's API do not list or find them).
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_PRIVATE_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_PRIVATE_INTERNAL_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#include "css_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The code point (U+10FFFD, the last private-use code point: an identifier code point, which the tokenizer reads in a name, that an author has no use for) that follows the two dashes of every
/// hidden name.
#define CSS_PRIV_MARK "\xF4\x8F\xBF\xBD"
#define CSS_PRIV_MARK_LEN 4u

/// Whether a custom property name is a hidden (private) one.
bool css_priv_hidden(const char * name, size_t length);

/// A style rule's private property names, and the scope it makes: the nearest enclosing scope is the `parent`.
typedef struct css_priv_scope {
  const struct css_priv_scope * parent;
  const GTEXT_CSS_String * names;   ///< Sorted, without repeats (arena).
  size_t nnames;
  const char * key;                 ///< The hidden name of a property is `key` and the name, whole (arena).
  size_t key_len;
} css_priv_scope;

/// The registration of a private property of a style rule: what an `@property` rule inside its `@private` rule says (css_at_property.c reads the rule; the compute context makes the
/// registration from this, as it does from a rule at the top of a sheet).
typedef struct {
  char * name;            ///< The hidden name (owned by the arena that holds the vector).
  size_t name_len;
  char * syntax;
  size_t syntax_len;
  bool has_syntax;
  bool inherits;
  char * initial;
  size_t initial_len;
  bool has_initial;
} css_priv_reg;

/// A growable list of registrations.
typedef struct {
  css_priv_reg * v;
  size_t n, cap;
} css_priv_regvec;

/// Whether an at-rule name is a conditional group rule that may hold an `@private` (and is read by the cascade as one).
bool css_priv_group_name(const GTEXT_CSS_String * name);

/// Read the `@private` rules of a style rule (the ones it holds directly and the ones in its conditional group rules): the names they define, as a scope inside `parent`, and the
/// registrations of the `@property` rules directly inside them (appended to `regs`; a later rule of a name replaces an earlier one). `*out` is NULL when the rule has no private name.
/// `serial` makes the hidden names unique within the sheet and `*epoch` among sheets (it is 0 until the sheet has a private name: this takes the sheet's number then). All memory is the arena's (and `regs`' vector the allocator's).
GTEXT_CSS_Status css_priv_scope_make(css_arena * arena, const GTEXT_Allocator * alloc, const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Rule * style, const css_priv_scope * parent,
    uint64_t * epoch, uint64_t serial, css_priv_regvec * regs, const css_priv_scope ** out);

/// The scope that has `name` in its private property names, nearest first (NULL: the name is not private here).
const css_priv_scope * css_priv_find(const css_priv_scope * chain, const char * name, size_t length);

/// The hidden name of `name` in `scope`, in the arena. NULL when memory ran out.
char * css_priv_name(css_arena * arena, const css_priv_scope * scope, const char * name, size_t length, size_t * out_length);

/// The text of a value with every reference to a private property written with the hidden name: the name a `var()` or an `inherit()` takes first, and a name in a `style()` query
/// (before the colon of a comparison or alone). `*out` is NULL when nothing needed a new name; otherwise it is a copy the allocator made (free it with the allocator).
GTEXT_CSS_Status css_priv_rewrite_text(const GTEXT_Allocator * alloc, const css_priv_scope * chain, const char * text, size_t length, char ** out, size_t * out_length);

/// The text with the sheet's number taken out of every hidden name in it (`--`, the code point, `p<number>_<rule>_` is `p_<rule>_`): what a dump of a sheet writes, so that the dumps of two
/// compiles of one text are one text. `*out` is a copy the allocator made (free it with the allocator).
GTEXT_CSS_Status css_priv_canonical(const GTEXT_Allocator * alloc, const char * text, size_t length, char ** out, size_t * out_length);

/// A new epoch for a sheet: process-wide, so that two sheets' hidden names differ.
uint64_t css_priv_next_epoch(void);

/// Free what a vector holds (the strings are the arena's).
void css_priv_regvec_free(const GTEXT_Allocator * alloc, css_priv_regvec * v);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_PRIVATE_INTERNAL_H
