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
 * A custom function as the evaluator reads it (css_custom_function.h is the public side). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_FNDEF_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_FNDEF_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_custom_function.h>

#include "css_creg.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// One parameter: its name, its type compiled (when it has one) and its default as text.
typedef struct {
  char * name;                     ///< `--name`, NUL-terminated.
  size_t name_len;
  css_compiled_syntax * type;      ///< The type, compiled; NULL when there is none.
  bool has_default;
  char * default_text;             ///< The default, trimmed, NUL-terminated; NULL when there is none.
  size_t default_len;
} css_fn_param;

/// A function the `@function` rules define.
typedef struct css_fn_def {
  GTEXT_Allocator alloc;
  char * name;                     ///< `--name`, NUL-terminated.
  size_t name_len;
  css_fn_param * params;
  size_t nparams;
  css_compiled_syntax * returns;   ///< The return type, compiled; NULL when there is none (`type(*)`).
  bool has_result;                 ///< The body has a `result` descriptor.
  const GTEXT_CSS_Rule * rule;     ///< The rule; its block is the body. Borrowed from the tree given to the set, or the set's own copy (`own_tree`).
  GTEXT_CSS_Id rule_id;            ///< The rule's id and span in the tree it was read from (kept for the diagnostics, since `rule` may be a copy).
  GTEXT_CSS_Span rule_span;
  char * own_text;                 ///< The text the copy was read from (for telling whether a rule changed), when there is a copy.
  size_t own_len;
  GTEXT_CSS_Tree * own_tree;       ///< The copy of the rule that `rule` is in, when the set was asked to own its rules (GTEXT_CSS_Function_Options::own_rules).
  uint32_t layer;                  ///< GTEXT_CSS_Layer_Id.
  size_t sheet;
} css_fn_def;

/// Whether two sets define the same functions the same way: the same names in the same layers with the same rules (the text of the rules the sets own, else the rules themselves) and
/// the same diagnostics counted. A NULL set defines none.
bool css_function_sets_equal(const GTEXT_CSS_Function_Set * x, const GTEXT_CSS_Function_Set * y);

/// The function of a name, or NULL. `work` (may be NULL) counts the probes.
const css_fn_def * css_function_set_find(const GTEXT_CSS_Function_Set * set, const char * name, size_t length, size_t * work);

/// Whether the prelude of an `@function` rule (with its block) is one: a name that is a function token of two dashes or more, parameters of the form `--name [type] [: default]` with no name twice,
/// and nothing but `returns <type>` after them. The sheet drops a rule that is not valid, as it does for every at-rule whose prelude does not match its grammar. GTEXT_CSS_OK, or GTEXT_CSS_E_OOM.
GTEXT_CSS_Status css_function_prelude_valid(const GTEXT_CSS_Registry * registry, const GTEXT_Allocator * alloc, const GTEXT_CSS_Rule * rule, bool * valid);

/// Free a definition.
void css_fn_def_free(css_fn_def * def);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_FNDEF_H
