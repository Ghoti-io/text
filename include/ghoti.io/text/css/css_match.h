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
 * @file css_match.h
 * @brief Matching compiled selectors against the caller's tree.
 *
 * Normative text: Selectors Level 4 (the matching of each selector), the DOM Standard (`:scope`, case rules), the HTML Standard (what HTML adds: the case rules of names and
 * of the values of a list of attributes, `:lang()`, `:dir()`), the Quirks Mode Standard (class and id case, the `:hover` and `:active` quirk), CSS Shadow Parts and CSS
 * Scoping (the shadow selectors), RFC 4647 (extended filtering for `:lang()`), as served 2026-10-28.
 *
 * ## The context
 *
 * A @ref GTEXT_CSS_Selector_Match_Context binds an element interface (css_element.h), options, and a **snapshot** of the tree: it is valid while the tree and the element states
 * do not change, owns its caches, is **reentrant and not shared between threads** (compiled selectors, style sheets and rule indexes are shared; a context is not). When the
 * tree changes, gtext_css_selector_match_reset() drops the caches.
 *
 * A selector that needs a callback the table lacks is never worked around: the match is GTEXT_CSS_E_UNSUPPORTED and the result names the callback.
 *
 * ## The cost model
 *
 * Matching goes right to left, and every step is memoized on (selector node, element) in the context. The matching of one complex selector against one element costs
 * O(|S| x (ancestors + preceding siblings)), and the matching of any number of selectors against all n elements of a snapshot costs at most (selector nodes) x n memo
 * entries **summed over the run**, never their product: `a b c d e` over a chain of n elements, `a ~ b ~ c` over n siblings, `:nth-child(odd of .x)` over n siblings (the
 * positions of the children of one parent are computed once and kept) and `:has()` of every combinator (the chain below a point depends on the point and the step and not
 * on the anchor, so it is memoized per step and element: each is visited once, O(m x n)). The one exception is a `:has()` that mentions the anchor itself (`:scope` or `&`),
 * which is not memoized across anchors and spends from a per-call budget. An **ancestor Bloom filter** of the ids, classes and local names of the ancestors, kept by
 * gtext_css_selector_match_walk(), lets a caller reject a descendant-combinator selector without matching it; it may make matching faster and never changes an answer, which the
 * tests assert by running with and without it and with and without memoization. The context counts its work (GTEXT_CSS_Selector_Match_Counters).
 *
 * ## Limits
 *
 * `max_memo_entries` (reaching it is GTEXT_CSS_E_LIMIT_MATCH_MEMO, never a wrong answer), and `max_work` per call
 * (GTEXT_CSS_E_LIMIT_MATCH_WORK). The evaluation recurses in the C stack once per compound of a complex selector (the parser bounds that, `max_compounds`) and per nesting of functional pseudo-classes (`max_depth`); a walk up a chain of ancestors
 * or along siblings, and a walk below a point, are iterations.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_MATCH_H
#define GHOTI_IO_GTEXT_CSS_CSS_MATCH_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The context of matching (the struct is private). */
typedef struct GTEXT_CSS_Selector_Match_Context GTEXT_CSS_Selector_Match_Context;

/** @brief What `:visited` and `:link` mean. */
typedef enum {
  GTEXT_CSS_VISITED_AS_STATE = 0, ///< The state bits say: `:link` and `:visited` are what the caller reports.
  GTEXT_CSS_VISITED_NEVER         ///< The privacy preserving reading: `:visited` never matches and `:link` matches any link (a visited one too).
} GTEXT_CSS_Visited_Mode;

/** @brief What a context may be told. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the default; it makes the context and its caches.
  /** An HTML document: a type selector and an attribute name are compared ASCII case-insensitively for an element in the HTML namespace (an element in another is compared
   *  exactly), and the values of the attributes the HTML Standard lists are compared ignoring case. An element whose namespace the interface does not give is taken as HTML. */
  bool html_document;
  /** Limited-quirks and quirks mode: class names and ids are compared ASCII case-insensitively, and a compound selector of nothing but `:hover` or `:active` matches only
   *  what `:any-link` matches (the Quirks Mode Standard, 4.1). */
  bool quirks;
  GTEXT_CSS_Visited_Mode visited; ///< See above. Default AS_STATE.
  /** The element `:scope` is; NULL: the root of the document (an element with no parent). For a query, the element it was called on. */
  GTEXT_CSS_Element_Handle scope;
  /** The element `:root` is, when `root_given`: the document element. Without it `:root` is an element with no parent (and no host), which is the root of a tree in a caller
   *  whose trees are all documents. A Markup tree has fragments and detached elements, whose top elements are not the root of any document. */
  GTEXT_CSS_Element_Handle root;
  bool root_given;
  /** The tree scope the selectors are matched for (`:host` is the element whose `shadow_scope` is it); NULL: none, and `:host` matches nothing. */
  const void * tree_scope;
  size_t max_memo_entries; ///< Most memo entries (48 bytes each, and the table is grown by doubling). Default 2,097,152 (2^21).
  size_t max_work;         ///< Most work in one call (compound tests and walk steps). Default 67,108,864 (2^26).
  bool disable_memo;       ///< For the tests: no memoization. The answers must not change.
  bool disable_bloom;      ///< For the tests: no ancestor filter. The answers must not change.
} GTEXT_CSS_Selector_Match_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Selector_Match_Options gtext_css_selector_match_options_default(void);

/** @brief What a context counted (the quadratic suite asserts bounds on these). */
typedef struct {
  size_t compounds_tested; ///< Compound selectors tested against an element.
  size_t memo_hits;        ///< Answers taken from the memo.
  size_t memo_entries;     ///< Entries in the memo now.
  size_t has_visits;       ///< Evaluations of a step of a `:has()` chain at an element.
  size_t nth_walks;        ///< Walks along a parent's children to find positions.
  size_t bloom_rejects;    ///< Selectors rejected by the ancestor filter.
  size_t rules_considered; ///< Rules the rule index tested (css_sheet.h).
  size_t work;             ///< Units of work in the last call.
} GTEXT_CSS_Selector_Match_Counters;

/** @brief What a match call reports. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;     ///< What the call returned.
  const char * message;        ///< A static description; never NULL after a call.
  const char * callback;       ///< GTEXT_CSS_E_UNSUPPORTED: the callback that was needed and absent (static), else NULL.
  const char * selector;       ///< GTEXT_CSS_E_UNSUPPORTED for a parse-only selector: its name (static), else NULL.
  bool matched;                ///< Some complex selector of the list matches.
  size_t complex_index;        ///< The complex selector that matched with the highest specificity (the first of equals).
  GTEXT_CSS_Specificity specificity; ///< Its specificity.
  GTEXT_CSS_Span span;         ///< A limit: the selector concerned.
} GTEXT_CSS_Selector_Match_Result;

/**
 * @brief Make a context.
 *
 * @param iface The element interface. Not NULL; copied (the callbacks and `user` must outlive the context).
 * @param options NULL for the defaults.
 * @param out Receives the context. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (no table, one smaller than its `user` field); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_match_context_new(const GTEXT_CSS_Element_Interface * iface, const GTEXT_CSS_Selector_Match_Options * options,
    GTEXT_CSS_Selector_Match_Context ** out);

/** @brief Free a context; NULL is ignored. */
GTEXT_API void gtext_css_selector_match_context_free(GTEXT_CSS_Selector_Match_Context * ctx);

/** @brief Drop the caches: the tree or the element states changed, and this is a new snapshot. The counters are kept. */
GTEXT_API void gtext_css_selector_match_reset(GTEXT_CSS_Selector_Match_Context * ctx);

/** @brief Change the `:scope` element and the tree scope of the options (the caches that depended on them are dropped). */
GTEXT_API void gtext_css_selector_match_set_scope(GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Handle scope, const void * tree_scope);

/** @brief The counters. */
GTEXT_API GTEXT_CSS_Selector_Match_Counters gtext_css_selector_match_counters(const GTEXT_CSS_Selector_Match_Context * ctx);

/** @brief Set the counters to zero (the memo stays). */
GTEXT_API void gtext_css_selector_match_counters_reset(GTEXT_CSS_Selector_Match_Context * ctx);

/**
 * @brief Match a list against an element.
 *
 * @param list The selectors. Not NULL.
 * @param ctx The context. Not NULL.
 * @param element The subject. Not NULL.
 * @param pseudo NULL for the element itself, or a list of one complex selector made only of pseudo-elements (`::before`, `::before::marker`) that is the pseudo-element
 *   asked for: only a selector that ends in exactly that chain matches. A pseudo-element never matches in a query.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK (whether it matched is `result->matched`); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED (a callback is absent: the result names it);
 *   GTEXT_CSS_E_LIMIT_MATCH_MEMO; GTEXT_CSS_E_LIMIT_MATCH_WORK; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_match(const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Selector_Match_Result * result);

/** @brief Called by gtext_css_selector_match_walk() for each element, in document order. Return false to stop. */
typedef bool (*GTEXT_CSS_Walk_Fn)(void * user, GTEXT_CSS_Element_Handle element);

/**
 * @brief Visit the elements of a subtree in document order, keeping the ancestor Bloom filter, so that the matches made from the visitor use it.
 *
 * Needs `first_child` and `next_sibling` (and `parent`, to build the filter for the ancestors of `root`). The walk is an iteration. The visitor may call
 * gtext_css_selector_match() on the element it is given, and on nothing that changes the tree.
 *
 * @param ctx The context.
 * @param root The first element; its descendants follow. Not NULL.
 * @param visitor Called for each. Not NULL.
 * @param user Passed to it.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED; GTEXT_CSS_E_LIMIT_MATCH_WORK; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_selector_match_walk(GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Handle root, GTEXT_CSS_Walk_Fn visitor, void * user,
    GTEXT_CSS_Selector_Match_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_MATCH_H
