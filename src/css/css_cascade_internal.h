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
 * What the cascade files share: the candidate set, the sort key, and the interface between the driver (css_cascade.c) and the part that decides one
 * property, with its rollbacks (css_revert.c). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CASCADE_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CASCADE_INTERNAL_H

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_cascade.h>
#include <ghoti.io/text/css/css_layer.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"
#include "css_registry_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The words of a sort key, most significant first. A larger word wins: a criterion that favors the smaller (the outer context for a normal declaration) is
/// stored inverted. The last word is the candidate's position, so that no two keys are equal.
enum {
  CK_TIER = 0,    ///< Origin and importance: 1 normal user-agent, 2 normal user, 3 presentational hints, 4 normal author, 5 animation, 6 important author .. 8 important user-agent, 9 transition.
  CK_CONTEXT,     ///< The context, inverted for a normal declaration.
  CK_ATTACHED,    ///< Element-attached.
  CK_LAYER,       ///< The layer, 0 for a presentational hint, inverted for an important declaration.
  CK_SPEC_A,
  CK_SPEC_B,
  CK_SPEC_C,
  CK_SCOPE,       ///< 0 without a scoping root, else fewer hops is larger.
  CK_SHEET,
  CK_RULE,
  CK_DECLARATION,
  CK_POSITION,
  CK_WORDS
};

#define TIER_NORMAL_UA 1u
#define TIER_NORMAL_USER 2u
#define TIER_HINT 3u
#define TIER_NORMAL_AUTHOR 4u
#define TIER_ANIMATION 5u
#define TIER_IMPORTANT_AUTHOR 6u
#define TIER_IMPORTANT_USER 7u
#define TIER_IMPORTANT_UA 8u
#define TIER_TRANSITION 9u

typedef enum { CAND_PROPERTY = 0, CAND_ALL, CAND_CUSTOM } cand_kind;

/// One candidate.
typedef struct {
  uint32_t k[CK_WORDS];            ///< The sort key.
  uint32_t context;                ///< The raw context.
  uint32_t layer_normal;           ///< The layer word as a normal declaration's (the lowest word of the layer's normal level).
  uint32_t group;                  ///< The add call it came from.
  uint32_t name_off, name_len;     ///< A custom property's name in the set's name buffer.
  uint32_t layer;                  ///< The layer id as given.
  uint32_t sheet, rule, declaration;
  uint32_t scope_distance;
  GTEXT_CSS_Specificity specificity;
  GTEXT_CSS_Id rule_id, declaration_id;
  GTEXT_CSS_Span span;
  GTEXT_CSS_Property_Id property, shorthand;
  GTEXT_CSS_Value_Set * set;       ///< Retained.
  uint8_t origin, level;           ///< The origin; and its level for `revert`: 0 user-agent, 1 user, 2 author, animation and transition.
  uint8_t kind, outcome, wide;
  bool important, hint, attached, scoped;
} cand;

typedef struct {
  GTEXT_CSS_Diagnostic_Kind kind;
  uint32_t name_off, name_len;
  GTEXT_CSS_Id declaration;
  GTEXT_CSS_Span span;
} cand_diag;

/// What the set knows of one context (css_cascade.h: gtext_css_candidates_use_layers()): the layer registry it was given, and whether it has a candidate.
typedef struct {
  uint32_t context;
  const GTEXT_CSS_Layer_Registry * layers;
  uint8_t occupied, used;
} ctx_entry;

struct GTEXT_CSS_Candidates {
  const GTEXT_CSS_Registry * registry;
  const GTEXT_CSS_Layer_Registry * layers;   ///< The registry of every context that has none of its own; may be NULL.
  ctx_entry * contexts;                      ///< An open-addressing table by context (a power of two long).
  size_t ncontexts, contexts_cap;
  GTEXT_Allocator alloc;
  GTEXT_CSS_Candidate_Options options;
  cand * items;
  size_t count, cap;
  char * names;
  size_t names_len, names_cap;
  cand_diag * diags;
  size_t ndiags, diag_cap, diag_total;
  uint32_t next_group;
  size_t work;                               ///< Probes of the table of contexts, for the quadratic suite.
  GTEXT_CSS_Property_Id all_id, custom_id;
  GCU_Atomic_Int refs;
  GCU_Atomic_Int sealed;
};

/// The candidate's name, as a string.
static inline const char * cand_name(const GTEXT_CSS_Candidates * s, const cand * c) {
  return s->names + c->name_off;
}

/// Compare two keys: -1, 0 or 1 as `a` is weaker than, equal to or stronger than `b`.
static inline int cand_key_cmp(const uint32_t * a, const uint32_t * b) {
  for (size_t i = 0; i < CK_WORDS; i++) {
    if (a[i] != b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }
  return 0;
}

/// The tier of the normal level that a candidate's layer belongs to (a hint's own, else its origin's).
static inline uint32_t cand_normal_tier(const cand * c) {
  if (c->hint) {
    return TIER_HINT;
  }
  return c->origin == GTEXT_CSS_ORIGIN_USER_AGENT ? TIER_NORMAL_UA : c->origin == GTEXT_CSS_ORIGIN_USER ? TIER_NORMAL_USER : TIER_NORMAL_AUTHOR;
}

static inline bool cand_rolls(const cand * c) {
  return c->outcome == GTEXT_CSS_PROP_RESULT_WIDE &&
      (c->wide == GTEXT_CSS_WIDE_REVERT || c->wide == GTEXT_CSS_WIDE_REVERT_LAYER || c->wide == GTEXT_CSS_WIDE_REVERT_RULE);
}

/// The state of one cascade, shared by the driver and the deciding part.
typedef struct {
  const GTEXT_CSS_Candidates * set;
  const cand * c;
  size_t n;
  GTEXT_Allocator alloc;
  size_t work;                     ///< Units of work done.
  size_t sort_work;                ///< ... of which the comparisons of the sorts (counted apart from the linear part).
  uint32_t * all;                  ///< The candidates of `all`, in the order added.
  size_t nall;
  uint32_t * all_sorted;           ///< ... in descending order of precedence; made on first need.
  uint32_t * buf_a, * buf_b;       ///< Scratch for sorting.
  size_t buf_cap;
  void * skip;                     ///< The table of what a walk has been told to skip (css_revert.c), reused by every property; NULL until needed.
  size_t skip_cap, skip_used;
  uint32_t skip_gen;
} cascade_run;

/// Decide one unit: its own candidates (indexes, in the order added) and, if `use_all`, those of `all`. `*winner` is the index, or UINT32_MAX for no declared value.
GTEXT_CSS_Status cascade_resolve(cascade_run * run, const uint32_t * own, size_t nown, bool use_all, uint32_t * winner, uint32_t * rollbacks);

/// The candidates of a unit in descending order of precedence (the own and, if `use_all`, `all`'s): `*out` is a new array of `*n` indexes (free it with the run's allocator).
GTEXT_CSS_Status cascade_sorted(cascade_run * run, const uint32_t * own, size_t nown, bool use_all, uint32_t ** out, size_t * n);

/// Stable merge sort of `a[0..n)` by `cmp`, with `tmp` of the same size. The comparison count is added to `*work`.
typedef int (*cascade_cmp)(const void * ctx, uint32_t x, uint32_t y);
void cascade_msort(uint32_t * a, uint32_t * tmp, size_t n, cascade_cmp cmp, const void * ctx, size_t * work);

/// The block's push (css_block.c).
GTEXT_CSS_Status css_block_push(GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property, const char * custom_name, size_t custom_length,
    bool important, GTEXT_CSS_Property_Outcome outcome, GTEXT_CSS_Wide_Keyword wide, const GTEXT_CSS_Value_Set * set, GTEXT_CSS_Property_Id shorthand,
    size_t declaration);

#define CASCADE_NONE UINT32_MAX

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CASCADE_INTERNAL_H
