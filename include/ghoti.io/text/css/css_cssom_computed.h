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
 * @brief `getComputedStyle()` and resolved values (CSS Object Model, "Extensions to the Window Interface" and "Resolved Values").
 *
 * gtext_cssom_computed_style() returns the **live, read-only** `CSSStyleDeclaration` that `getComputedStyle(element, pseudo)` returns (css_cssom_decl.h: owner COMPUTED;
 * every mutator is GTEXT_CSS_E_NO_MODIFICATION and the `cssText` getter is the empty string, as the served text says: "If the computed flag is set, then return the empty string").
 *
 * ## When the declaration is empty
 *
 * Served step 5: the declarations are those of the resolved values "If obj is not null, and elt is connected, part of the flat tree, and its shadow-including root has a browsing
 * context which either doesn't have a browsing context container, or whose browsing context container is being rendered"; otherwise `decls` stays the empty list. Here the three
 * facts are the bits of GTEXT_CSSOM_Config::element_state (absent: all true) and the layout interface's `is_rendered` (absent: rendered). A `display: none` element is connected and
 * in the tree, so it has its values. A pseudo-element argument is a string that starts with a colon and parses as a pseudo-element selector; anything else is read as "no
 * pseudo-element", and a `::slotted()` or `::part()` argument makes `obj` null, which is the empty declaration.
 *
 * ## What it holds
 *
 * Every supported longhand in lexicographic order, then the element's custom properties whose computed value is not the guaranteed-invalid value, also in lexicographic order. Each
 * value is the property's **resolved value** (tools/css/resolved_value.tsv): the computed value, except for the properties whose category (CSSOM section 9) says the used value: the
 * colors (`currentcolor` replaced by the element's `color`), and the sizes, margins, paddings, insets, `line-height` and a few others, which are layout's (GTEXT_CSSOM_Layout_Interface).
 * A shorthand reads from the resolved longhands (gtext_css_block_shorthand_value()); `getPropertyPriority` is always empty.
 *
 * ## Live
 *
 * It is live in the way the platform means: every call checks the document's generation and the caller's tree version against those it last read for, and reads again when either
 * moved; it is a pure function of the document and the tree, so two reads at one state are byte-identical. A document with neither a tree version hook nor a notification disables
 * the caching across calls: each call computes its chain again (gtext_cssom_document_counters(): `computed_computes`).
 *
 * `:visited` never matches in this library, so the privacy rule of `getComputedStyle()` has nothing to hide.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CSSOM_COMPUTED_H
#define GHOTI_IO_GTEXT_CSS_CSS_CSSOM_COMPUTED_H

#include <ghoti.io/text/css/css_cssom_core.h>
#include <ghoti.io/text/css/css_cssom_decl.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Where the resolved value of a property came from (gtext_cssom_computed_origin()). */
typedef enum {
  GTEXT_CSSOM_ORIGIN_COMPUTED = 0,   ///< The computed value: the property's category says so, or its used value does not apply (the layout said so, or `display` is `none` or `contents`).
  GTEXT_CSSOM_ORIGIN_USED,           ///< The used value: from the layout interface, or made here by the property's rule (a color with `currentcolor` replaced, an `auto` that resolves to zero).
  GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT   ///< The category asks for the used value, layout was not asked (no `used_value` callback) and the computed value stands in.
} GTEXT_CSSOM_Origin;

/**
 * @brief `getComputedStyle(element, pseudo)`.
 *
 * @param document The document. Not NULL.
 * @param element The caller's element handle (the element interface knows it). Not NULL.
 * @param pseudo NULL or empty for the element; else a pseudo-element selector as a string (`::before`, `:after`). Not copied beyond the call.
 * @param pseudo_length Its length.
 * @param out Receives a new declaration (a new object each call, as `[NewObject]` says) with one external reference; written only on success. Not NULL.
 * @return GTEXT_CSS_OK (also for an element that is not rendered: the declaration is empty); GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_UNSUPPORTED (the document has no element
 *   interface); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_CSSOM. Computing happens on the first read, whose statuses are those of the read (gtext_cssom_decl_length() and the others): GTEXT_CSS_E_UNSUPPORTED
 *   when a callback of the element interface or the layout was needed and absent or failed that way (gtext_cssom_computed_failure() names it), and the limits of the computed layer.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_computed_style(GTEXT_CSSOM_Document * document, const void * element, const char * pseudo, size_t pseudo_length, GTEXT_CSSOM_Decl ** out);

/**
 * @brief Whether the declaration is empty because of the first steps (the element is not connected, not in the flat tree, not rendered, or the pseudo-element is `::slotted()` or `::part()` or not one).
 * @param decl A computed declaration. Not NULL.
 * @param empty Receives the answer. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a declaration that is not a computed one); the statuses of a read.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_computed_is_empty(GTEXT_CSSOM_Decl * decl, bool * empty);

/**
 * @brief Where the resolved value of a property of a computed declaration came from. A shorthand is USED if any longhand is, else ABSENT_LAYOUT if any is, else COMPUTED.
 * @param decl A computed declaration. Not NULL.
 * @param name The property (a longhand, a shorthand, or a custom property, which is COMPUTED).
 * @param length Its length.
 * @param origin Receives the origin. Not NULL.
 * @param found Receives whether the declaration has the property (false for an empty declaration or an unsupported property: then `origin` is COMPUTED). Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; the statuses of a read.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_computed_origin(GTEXT_CSSOM_Decl * decl, const char * name, size_t length, GTEXT_CSSOM_Origin * origin, bool * found);

/**
 * @struct GTEXT_CSSOM_Computed_Failure
 * @brief What the last failed read of a computed declaration was missing: the callback (`attribute`, `used_value` ...) or the property, as the layer that failed names them.
 */
typedef struct {
  GTEXT_CSS_Status status;          ///< The status that read failed with, or GTEXT_CSS_OK when none has.
  char callback[96];                ///< The callback or the thing that was needed, NUL-terminated ("used_value", "attribute", a unit); empty if none.
  GTEXT_CSS_Property_Id property;   ///< The property being read, or 0.
} GTEXT_CSSOM_Computed_Failure;

/** @brief The last failure of a read of a computed declaration of the document (it is overwritten by the next failure). @return GTEXT_CSS_OK, or GTEXT_CSS_E_INVALID. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_computed_failure(const GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Computed_Failure * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_COMPUTED_H
