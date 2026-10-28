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
 * @file markup_query.h
 * @brief `querySelector()`, `querySelectorAll()`, `matches()` and `closest()` on a Markup tree.
 *
 * Reference: the DOM Standard (ParentNode.querySelector, Element.matches and Element.closest), Selectors 4, and the HTML Standard for the case rules, as served 2026-10-28.
 *
 * The selector is parsed as an unforgiving list of complex selectors (css_selector.h), with no namespace prefix declared: a syntax error is
 * GTEXT_MARKUP_E_SELECTOR, with the diagnostic in the result. `:scope` is the element the call is on (the root element for a document or a fragment), the results are
 * the descendants of the scope and not the scope itself, `closest()` is inclusive of the element, pseudo-elements never match, and `:visited` never matches while `:link` matches
 * any link (the privacy preserving reading). The document's mode decides the case rules and the quirks (css_markup.h). A query over n elements with a selector of m nodes is
 * O(m x n) by the cost model of css_match.h. Each function also takes a compiled selector list (`_compiled`), so that a selector is parsed once and used many times.
 *
 * The tree must not change during a call.
 */

#ifndef GHOTI_IO_GTEXT_MARKUP_MARKUP_QUERY_H
#define GHOTI_IO_GTEXT_MARKUP_MARKUP_QUERY_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_markup.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_core.h>
#include <ghoti.io/text/markup/markup_tree.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What a query may be told. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the default.
  GTEXT_CSS_Markup_Kind kind;         ///< The language of the document (GTEXT_CSS_MARKUP_AUTO: from the document element).
  size_t max_memo_entries;            ///< The matcher's limit (css_match.h); 0 is its default.
  size_t max_work;                    ///< The matcher's limit for a call; 0 is its default.
  /** More state bits for an element (GTEXT_CSS_STATE_*), OR-ed with the ones the markup decides (css_markup.h); NULL for none. */
  uint64_t (*state)(void * user, const GTEXT_Markup_Node * element);
  void * user;                        ///< Passed to \`state\`.
  GTEXT_CSS_Selector_Options selector; ///< The limits of parsing a selector given as text (`mode` and `namespaces` are ignored); gtext_markup_query_options_default() has the defaults.
} GTEXT_Markup_Query_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_Markup_Query_Options gtext_markup_query_options_default(void);

/** @brief What a query reports. Zero it before the first use; free it with gtext_markup_query_result_free(). */
typedef struct {
  GTEXT_Markup_Status status;          ///< What the call returned.
  const char * message;                ///< A static description; never NULL after a call.
  GTEXT_CSS_Status css_status;         ///< The status of the selector layer behind it (a limit, GTEXT_CSS_E_UNSUPPORTED naming a selector in `selector`...).
  GTEXT_CSS_Selector_Result selector;  ///< GTEXT_MARKUP_E_SELECTOR: the diagnostics of the selector's syntax error.
  const char * unsupported;            ///< GTEXT_CSS_E_UNSUPPORTED: the callback or the selector that cannot be answered (static), else NULL.
} GTEXT_Markup_Query_Result;

/** @brief Release what a result holds and reset it. Safe on NULL and on a zeroed result. */
GTEXT_API void gtext_markup_query_result_free(GTEXT_Markup_Query_Result * result);

/**
 * @brief The first descendant element of `scope` that a selector matches, in document order.
 *
 * @param scope A document, a fragment or an element. Not NULL.
 * @param selector The selector text, UTF-8.
 * @param length Its length.
 * @param options NULL for the defaults.
 * @param out Receives the element, or NULL when there is none. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_MARKUP_OK; GTEXT_MARKUP_E_INVALID; GTEXT_MARKUP_E_SELECTOR (a syntax error: no result); GTEXT_MARKUP_E_LIMIT (a limit of the parse or the matcher); GTEXT_MARKUP_E_OOM.
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_query_selector(const GTEXT_Markup_Node * scope, const char * selector, size_t length,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result);

/** @brief gtext_markup_query_selector() with a compiled selector. */
GTEXT_API GTEXT_Markup_Status gtext_markup_query_selector_compiled(const GTEXT_Markup_Node * scope, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result);

/**
 * @brief Every descendant element of `scope` that a selector matches, in document order, each once.
 *
 * @param out Receives a new array (NULL for none), freed with gtext_markup_query_free(). Not NULL.
 * @param count Receives its length. Not NULL.
 * @return As gtext_markup_query_selector().
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_query_selector_all(const GTEXT_Markup_Node * scope, const char * selector, size_t length,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node *** out, size_t * count, GTEXT_Markup_Query_Result * result);

/** @brief gtext_markup_query_selector_all() with a compiled selector. */
GTEXT_API GTEXT_Markup_Status gtext_markup_query_selector_all_compiled(const GTEXT_Markup_Node * scope, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node *** out, size_t * count, GTEXT_Markup_Query_Result * result);

/** @brief Release an array of gtext_markup_query_selector_all(); NULL is ignored. `options` is the one the query had (its allocator), or NULL. */
GTEXT_API void gtext_markup_query_free(const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** nodes);

/** @brief Called for each match, in document order. Return false to stop. */
typedef bool (*GTEXT_Markup_Query_Fn)(void * user, const GTEXT_Markup_Node * element);

/** @brief The matches one by one, with no array: gtext_markup_query_selector_all() as an iteration. */
GTEXT_API GTEXT_Markup_Status gtext_markup_query_selector_each(const GTEXT_Markup_Node * scope, const char * selector, size_t length,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Query_Fn fn, void * user, GTEXT_Markup_Query_Result * result);

/** @brief gtext_markup_query_selector_each() with a compiled selector. */
GTEXT_API GTEXT_Markup_Status gtext_markup_query_selector_each_compiled(const GTEXT_Markup_Node * scope, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Query_Fn fn, void * user, GTEXT_Markup_Query_Result * result);

/**
 * @brief Whether an element is matched by a selector, with `:scope` the element itself.
 *
 * @param element An element. Not NULL.
 * @param out Receives the answer. Not NULL.
 * @return As gtext_markup_query_selector().
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_matches(const GTEXT_Markup_Node * element, const char * selector, size_t length,
    const GTEXT_Markup_Query_Options * options, bool * out, GTEXT_Markup_Query_Result * result);

/** @brief gtext_markup_matches() with a compiled selector. */
GTEXT_API GTEXT_Markup_Status gtext_markup_matches_compiled(const GTEXT_Markup_Node * element, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, bool * out, GTEXT_Markup_Query_Result * result);

/**
 * @brief The element itself or the nearest ancestor element that a selector matches, with `:scope` the element the call is on.
 *
 * @param out Receives the element, or NULL when there is none. Not NULL.
 * @return As gtext_markup_query_selector().
 */
GTEXT_API GTEXT_Markup_Status gtext_markup_closest(const GTEXT_Markup_Node * element, const char * selector, size_t length,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result);

/** @brief gtext_markup_closest() with a compiled selector. */
GTEXT_API GTEXT_Markup_Status gtext_markup_closest_compiled(const GTEXT_Markup_Node * element, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_MARKUP_MARKUP_QUERY_H
