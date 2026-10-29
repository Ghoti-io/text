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
  GTEXT_CSS_Container_Interface container; ///< A copy of the caller's; `size` 0 when the caller gave none.
  size_t max_requests;
};

typedef struct {
  const struct GTEXT_CSS_Media_List_Internal * in;
  uint32_t node;
  uint32_t idx;
  int32_t def; ///< >= 0: a custom media definition (node is MQ_NONE and idx is the query of its list).
  uint8_t acc;
} eframe;

/// A request for a query container: a name (NULL: any) and the kinds of feature asked (MQC_*).
typedef struct {
  char * name;      ///< Owned by the call (a copy: the query the name was read from may be freed while the call lives on).
  size_t name_len;
  uint32_t mask;
} cq_request;

/// The query container of an element for a request (NULL: none), the memo of a walk up the ancestors.
typedef struct {
  const void * element;
  uint32_t request;     ///< Index into the request table, plus one (0: the slot is empty).
  const void * container;
} cq_cache_entry;

/// The answer of a container condition for a container: (container, root node) to a three-valued result.
typedef struct {
  const void * container;
  size_t serial;        ///< The serial of the list the root is of (0: the slot is empty); never an address, which a freed list leaves for the next one.
  uint32_t root;        ///< The node of the root query in that list.
  uint8_t result;
} cq_result_entry;

/// The answer of a supports declaration for the evaluation in progress, by its text: a declaration that a condition repeats is parsed once.
typedef struct {
  uint64_t hash;
  char * key;       ///< name, a NUL, value (the call's own copy); NULL: the slot is empty.
  size_t key_len;
  uint8_t result;   ///< 1 supported, 0 not.
} cq_decl_entry;

struct GTEXT_CSS_Condition_Call_Internal {
  GTEXT_Allocator alloc;
  cq_decl_entry * decls;
  size_t decls_cap, decls_used;
  const void * owner; ///< The defs the memo is for.
  uint8_t * memo;     ///< Per winning definition: 0 not yet, 1 true, 2 false.
  size_t memo_n;
  eframe * stack;
  size_t cap;
  cq_request * reqs;  ///< The distinct requests of the call.
  size_t nreq, capreq;
  cq_cache_entry * cache;  ///< Open addressing by (element, request).
  size_t cache_used, cache_cap;
  cq_result_entry * results;
  size_t results_used, results_cap;
  const void ** path;  ///< Scratch of a walk up the ancestors.
  size_t path_cap;
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
bool css_decl_memo_get(const struct GTEXT_CSS_Condition_Call_Internal * ci, const char * name, size_t nl, const char * value, size_t vl, bool * result);
GTEXT_CSS_Status css_decl_memo_put(struct GTEXT_CSS_Condition_Call_Internal * ci, const char * name, size_t nl, const char * value, size_t vl, bool result);
void css_decl_memo_clear(struct GTEXT_CSS_Condition_Call_Internal * ci);
void css_decl_memo_free(struct GTEXT_CSS_Condition_Call_Internal * ci);
GTEXT_CSS_Tristate css_supports_eval_leaf(const GTEXT_CSS_Conditions * c, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail);

/// One media feature node against an environment. A failure is in *status (and *detail), the answer is then meaningless.
GTEXT_CSS_Tristate css_mq_eval_feature(const GTEXT_CSS_Media_Environment * e, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status,
    const char ** detail);

/// Evaluate the query node `qnode` of `list`; the call's memo must have been prepared with css_conditions_prepare_call().
GTEXT_CSS_Status css_conditions_eval_query(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const struct GTEXT_CSS_Media_List_Internal * list, uint32_t qnode,
    GTEXT_CSS_Tristate * out);

struct css_cq_ctx;

/// Evaluate the query node `qnode` of `list` with a query container (`cq` NULL: a media query or a supports condition).
GTEXT_CSS_Status css_conditions_eval_query_cq(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const struct GTEXT_CSS_Media_List_Internal * list,
    uint32_t qnode, struct css_cq_ctx * cq, GTEXT_CSS_Tristate * out);

/// A size feature node against a query container; a style, scroll-state or anchored feature node. Failures are in *status and *detail.
GTEXT_CSS_Tristate css_cq_eval_size(struct css_cq_ctx * cq, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail);
GTEXT_CSS_Tristate css_cq_eval_sub(struct css_cq_ctx * cq, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail);

/// Release what a call holds for containers (css_container.c).
void css_cq_call_free(struct GTEXT_CSS_Condition_Call_Internal * ci);

/// Make the call ready for `c` (allocate its internal block, size the memo to the definitions).
GTEXT_CSS_Status css_conditions_prepare_call(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call);

#ifdef __cplusplus
}
#endif

#endif
