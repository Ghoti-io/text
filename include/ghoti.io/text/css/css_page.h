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
 * @file css_page.h
 * @brief `@page`: the page selector, its specificity, and whether it matches a page. Pure functions of (page name, page position, blank, side).
 *
 * Normative text: CSS Paged Media 3 (the page selectors, 4.4 "Cascading in the page context"), as served 2026-10-29. `<page-selector> = [ <ident-token>? <pseudo-page>* ]!` with
 * `<pseudo-page> = : [ left | right | first | blank ]`, as the pin has it. The page context is the caller's: the library lays out no page.
 *
 * "The specificity of a page selector is computed in a manner analogous to the computations defined in the Selectors module: count the number of page type names (= f; it can only be 0 or 1),
 * the number of `:first` or `:blank` pseudo-classes (= g), the number of `:left` or `:right` (= h). Repeated occurrences of the same pseudo-classes are allowed and do increase specificity.
 * ... values higher than the limit must be clamped to that limit, and not overflow."
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_PAGE_H
#define GHOTI_IO_GTEXT_CSS_CSS_PAGE_H

#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The specificity of a page selector, compared in the order f, g, h. */
typedef struct {
  uint32_t f; ///< Page type names (0 or 1).
  uint32_t g; ///< `:first` and `:blank`.
  uint32_t h; ///< `:left` and `:right`.
} GTEXT_CSS_Page_Specificity;

/** @brief One page selector: an optional page type name and pseudo-pages (the counts keep a repeated one). The strings are owned by the rule that holds the selector. */
typedef struct {
  GTEXT_CSS_String name;     ///< The page type name, decoded (data NULL: none).
  uint32_t first;            ///< How many `:first`.
  uint32_t blank;            ///< How many `:blank`.
  uint32_t left;             ///< How many `:left`.
  uint32_t right;            ///< How many `:right`.
  GTEXT_CSS_Page_Specificity specificity;
} GTEXT_CSS_Page_Selector;

/** @brief Whether a page is a left or a right page (decided by the caller from the page's position and the document's direction). */
typedef enum { GTEXT_CSS_PAGE_SIDE_RIGHT = 0, GTEXT_CSS_PAGE_SIDE_LEFT } GTEXT_CSS_Page_Side;

/** @brief The page a selector is matched against. */
typedef struct {
  GTEXT_CSS_String name;     ///< The page's type name (the `page` property's value); data NULL or length 0: the page has none. Compared case-sensitively.
  bool first;                ///< The first page (of the document, or of its named page group: the caller's reading of "first").
  bool blank;                ///< A blank page.
  GTEXT_CSS_Page_Side side;  ///< Left or right.
} GTEXT_CSS_Page_Context;

/** @brief -1, 0, 1: a is less specific than, as specific as, more specific than b. */
GTEXT_API int gtext_css_page_specificity_compare(GTEXT_CSS_Page_Specificity a, GTEXT_CSS_Page_Specificity b);

/** @brief Whether one selector matches a page: its name equals the page's, and each pseudo-page holds. */
GTEXT_API bool gtext_css_page_selector_matches(const GTEXT_CSS_Page_Selector * selector, const GTEXT_CSS_Page_Context * page);

/**
 * @brief Whether a selector list (the prelude of `@page`; the empty list matches every page) matches a page, and the specificity of the most specific selector of it that does.
 *
 * @param selectors The list; NULL with a count of 0 matches any page with specificity (0,0,0).
 * @param count Its length.
 * @param page The page. Not NULL.
 * @param specificity NULL, or receives the highest specificity among the matching selectors.
 */
GTEXT_API bool gtext_css_page_matches(const GTEXT_CSS_Page_Selector * selectors, size_t count, const GTEXT_CSS_Page_Context * page, GTEXT_CSS_Page_Specificity * specificity);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_PAGE_H
