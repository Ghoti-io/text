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
 * What the conditions files share (css_conditions.c, css_media_eval.c). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_CONDITION_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_CONDITION_INTERNAL_H

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/macros.h>

#include "css_media_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CSS_DEF_LIST 0
#define CSS_DEF_TRUE 1
#define CSS_DEF_FALSE 2

typedef struct {
  char * name;
  size_t name_length;
  uint8_t kind; ///< CSS_DEF_*.
  GTEXT_CSS_Media_List * list;
  uint32_t order; ///< The order of addition.
} css_def;

struct GTEXT_CSS_Condition_Defs {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  bool sealed;
  css_def * items;
  size_t count, cap;
  /* After sealing: the winners, sorted by name. */
  uint32_t * winner;   ///< Indexes into items.
  size_t winner_count;
  uint8_t * cyclic;    ///< Per winner.
  size_t cycle_count;
  uint32_t * cycles;   ///< The winners on a cycle, in the order of the definitions.
};

struct GTEXT_CSS_Conditions {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  const GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Media_Environment env;
  bool custom_media;
  size_t max_work;
  GTEXT_CSS_Support_Policy policy;
  const GTEXT_CSS_Element_Interface * iface;
  struct GTEXT_CSS_Condition_Defs * defs; ///< Sealed; never NULL (an empty one stands for none).
};

typedef struct {
  const struct GTEXT_CSS_Media_List_Internal * in;
  uint32_t node;
  uint32_t idx;
  int32_t def; ///< >= 0: a custom media definition (node is MQ_NONE and idx is the query of its list).
  uint8_t acc;
} eframe;

struct GTEXT_CSS_Condition_Call_Internal {
  GTEXT_Allocator alloc;
  const void * owner; ///< The defs the memo is for.
  uint8_t * memo;     ///< Per winning definition: 0 not yet, 1 true, 2 false.
  size_t memo_n;
  eframe * stack;
  size_t cap;
};

/// The winner index of a name, or -1.
int css_defs_find(const struct GTEXT_CSS_Condition_Defs * d, const char * name, size_t length);
bool css_defs_is_cyclic(const struct GTEXT_CSS_Condition_Defs * d, size_t winner);
int css_defs_kind(const struct GTEXT_CSS_Condition_Defs * d, size_t winner);
const struct GTEXT_CSS_Media_List_Internal * css_defs_list(const struct GTEXT_CSS_Condition_Defs * d, size_t winner);

GTEXT_CSS_Tristate css_tri_not(GTEXT_CSS_Tristate a);
GTEXT_CSS_Tristate css_tri_and(GTEXT_CSS_Tristate a, GTEXT_CSS_Tristate b);
GTEXT_CSS_Tristate css_tri_or(GTEXT_CSS_Tristate a, GTEXT_CSS_Tristate b);

/// One supports leaf (a declaration, selector(), font-tech() ...) against the conditions.
GTEXT_CSS_Tristate css_supports_eval_leaf(const GTEXT_CSS_Conditions * c, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail);

/// One media feature node against an environment. A failure is in *status (and *detail), the answer is then meaningless.
GTEXT_CSS_Tristate css_mq_eval_feature(const GTEXT_CSS_Media_Environment * e, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status,
    const char ** detail);

/// Evaluate the query node `qnode` of `list`; the call's memo must have been prepared with css_conditions_prepare_call().
GTEXT_CSS_Status css_conditions_eval_query(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const struct GTEXT_CSS_Media_List_Internal * list, uint32_t qnode,
    GTEXT_CSS_Tristate * out);

/// Make the call ready for `c` (allocate its internal block, size the memo to the definitions).
GTEXT_CSS_Status css_conditions_prepare_call(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call);

#ifdef __cplusplus
}
#endif

#endif
