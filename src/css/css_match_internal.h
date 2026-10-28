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
 * What the matcher's files share: the context, the memo, and the evaluation functions of one file that another calls. Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_MATCH_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_MATCH_INTERNAL_H

#include <ghoti.io/text/css/css_match.h>
#include <ghoti.io/text/macros.h>

#include "css_selector_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef GTEXT_CSS_Element_Handle El;

/// What `&` stands for: the list of the parent rule and the context that list is read in; NULL list: the `:scope` element.
typedef struct css_nest {
  const GTEXT_CSS_Selector_List * list;
  const struct css_nest * parent;
} css_nest;

/// What a memo entry is the answer to.
enum {
  MK_M = 1,   ///< The element matches the compound of the step and the chain to its left.
  MK_D,       ///< Some proper ancestor of the element is an M of the step.
  MK_S,       ///< Some earlier sibling of the element is an M of the step.
  MK_G,       ///< :has(): the element matches the compound of the step and the chain to its right exists below it.
  MK_H,       ///< :has(): some descendant of the element is a G of the step.
  MK_L,       ///< :has(): some later sibling of the element is a G of the step.
  MK_NTH,     ///< The positions of the element among its siblings (`a` from the start, `b` from the end).
  MK_NTHF     ///< The same among the siblings that match a selector list, or are of a type.
};

/// One entry of the memo.
typedef struct {
  size_t uid;       ///< The parse (list) the node belongs to; 0 with val 0: an empty slot.
  const void * elem;
  const void * aux; ///< The scope element the answer depends on (a selector that mentions the scope), else NULL.
  const void * ts;  ///< The tree scope the answer was made in (a sheet shared by two shadow trees is matched in each).
  uint32_t step;    ///< The node's id (or the node's id and the step of a chain).
  uint16_t kind;
  uint16_t val;     ///< 0 empty, 1 false, 2 true, 3 positions known.
  uint32_t a, b;    ///< NTH: the positions.
} css_memo_ent;

struct GTEXT_CSS_Selector_Match_Context {
  GTEXT_CSS_Element_Interface iface; ///< A copy, zero past the size the caller gave.
  GTEXT_CSS_Selector_Match_Options opt;
  GTEXT_Allocator alloc;
  uint64_t avail;                    ///< A bit for each callback the table has.
  css_memo_ent * tab;
  size_t cap;
  size_t used;
  GTEXT_CSS_Selector_Match_Counters ctr;
  GTEXT_CSS_Status status;           ///< Sticky: the first failure of the call.
  const char * missing;
  const char * unsupported;
  GTEXT_CSS_Span fail_span;
  size_t max_work;
  El scope;                          ///< The `:scope` element now (NULL: the root).
  const void * tree_scope;
  const css_nest * nest;
  const void * memo_aux;             ///< The aux key now: the scope element while a scope-dependent selector is read, else NULL.
  /* :has() */
  El has_anchor;
  size_t has_budget;
  size_t has_count;
  bool has_dependent;
  /* the ancestor filter */
  uint8_t * bloom;
  bool bloom_live;
  El bloom_for;                      ///< The element the filter holds the ancestors of: the one being visited. A match of another element ignores the filter.
  size_t bloom_depth;
  /* scratch for the walks up the ancestors and along the siblings */
  void ** path;
  uint8_t * pathv;
  size_t path_cap;
};

/// Whether the element interface of the context has a callback.
#define CSS_HAVE(ctx, cb) (((ctx)->avail >> (cb)) & 1u)

/* ---- the memo --------------------------------------------------------------------------------------------- */

/// Look an entry up. true: found (in `*out`, a copy).
bool css_memo_get(struct GTEXT_CSS_Selector_Match_Context * ctx, size_t uid, uint16_t kind, uint32_t step, const void * elem, const void * aux, css_memo_ent * out);

/// Store (insert or overwrite) an entry; false (and the context failed with GTEXT_CSS_E_LIMIT_MATCH_MEMO or OOM) if it could not be kept.
bool css_memo_put(struct GTEXT_CSS_Selector_Match_Context * ctx, size_t uid, uint16_t kind, uint32_t step, const void * elem, const void * aux, uint16_t val, uint32_t a,
    uint32_t b);

/// Drop every entry.
void css_memo_clear(struct GTEXT_CSS_Selector_Match_Context * ctx);

/* ---- failing and counting ---------------------------------------------------------------------------------- */

/// Fail the call (the first failure wins) and return false.
bool css_match_fail(struct GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Status st);

/// A callback the table lacks.
bool css_match_missing(struct GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Callback cb);

/// Count one unit of work; false (and the call failed) past the limit.
bool css_match_work(struct GTEXT_CSS_Selector_Match_Context * ctx);

/* ---- the evaluation, across files -------------------------------------------------------------------------- */

/// Whether `element` matches some complex selector of `list` (an `:is()` of it), with no pseudo-element asked for.
bool css_match_any(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, El element);

/// Whether `element` matches a compound selector (not the chain to its left).
bool css_match_compound(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Compound * cmp, El element, size_t limit);

/// Whether `element` matches the compound of a complex selector at `index` and the chain to its left (a memoized M).
bool css_match_chain(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * cx, size_t index, El element);

/// The `:nth-*` pseudo-classes (css_match_nth.c): the positions are counted once for each parent.
bool css_match_nth(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El element);

/// `:first-of-type` and kin, with the type taken from the element itself.
bool css_match_of_type(struct GTEXT_CSS_Selector_Match_Context * ctx, uint16_t row, El element);

/// `:has()` (css_match_has.c).
bool css_match_has(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El element);

/// RFC 4647 extended filtering, ASCII case-insensitive: whether the language `tag` is in the language `range`.
bool css_lang_range_matches(const char * range, size_t rlen, const char * tag, size_t tlen);

/// `:lang()`.
bool css_match_lang(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El element);

/// Element name comparison under the document's case rules.
bool css_match_is_html(struct GTEXT_CSS_Selector_Match_Context * ctx, El element);

/// The ancestor filter (css_match_bloom.c).
void css_bloom_clear(struct GTEXT_CSS_Selector_Match_Context * ctx);
bool css_bloom_push(struct GTEXT_CSS_Selector_Match_Context * ctx, El element);
bool css_bloom_pop(struct GTEXT_CSS_Selector_Match_Context * ctx, El element);
/// false: the filter says the complex selector cannot match an element with the ancestors the filter holds.
bool css_bloom_may_match(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Complex * cx);

/// The inclusive flat-tree parent used by `:host-context()`: the assigned slot, else the parent, else the host of the shadow tree.
El css_flat_parent(struct GTEXT_CSS_Selector_Match_Context * ctx, El element);

/// The public match of one complex selector, with the shadow selectors' overrides (the rule index passes the slot and the host it found).
bool css_match_complex_full(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * cx, El element,
    const GTEXT_CSS_Complex * request, El slot, El host);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_MATCH_INTERNAL_H
