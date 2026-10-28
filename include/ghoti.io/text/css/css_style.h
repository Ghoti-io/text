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
 * @file css_style.h
 * @brief One call from an element to its computed style: its `style` attribute, its presentation attributes and the declarations a caller adds,
 *   cascaded and computed.
 *
 * Reference: CSS Cascade 5 (the cascade, 6.5 presentational hints), SVG 2 (6.6 presentation attributes, 5.6 `<use>`), CSSOM (resolved value) and
 * the parts this assembles: css_presentation.h, css_cascade.h, css_computed.h and css_custom.h.
 *
 * ## The call
 *
 * gtext_css_style_compute() takes the context (css_computed.h: the registry, the environment, the caller's element interface and the limits; it is
 * reentrant, and any number of threads may compute with one), an element, the computed style of the **inheritance parent**, and optionally more
 * declarations. It
 * 1. reads the element's presentation attributes through the element interface (the table of css_presentation.h) and its `style` attribute,
 * 2. adds the declaration lists of the entries the caller gives (the declarations of the style rules that matched, once selectors exist: each with the
 *    sort keys of css_cascade.h),
 * 3. cascades them, and
 * 4. computes the style from the winners, the parent's style and the environment.
 *
 * The parent is the style of the element **inherited from**, which is the tree parent for most elements and the `<use>` element for the clone of an
 * element that the `<use>` instantiates (SVG 2): the caller passes the style it computed for that element, and this call asks the tree nothing about it.
 * A chain of n elements costs O(n): a style shares what it inherits with its parent's (css_computed.h, "Values are shared"), and nothing is copied.
 *
 * ## What the style is
 *
 * The result is a @ref GTEXT_CSS_Computed_Style: **immutable and reference counted** (gtext_css_computed_retain(), gtext_css_computed_free(); the counts
 * are atomic, cutil's), readable from any number of threads. Besides the computed values it keeps the cascade's winners, so that gtext_css_computed_specified()
 * gives the **specified value** of a property with where it came from (origin, importance, the span in its source), and the diagnostics of what was dropped. It
 * reads, in the registry's order or by name, any property of the registry (gtext_css_computed_get(), gtext_css_computed_next()); a property the engine
 * cannot compute is GTEXT_CSS_E_UNSUPPORTED and the result names it.
 *
 * ## Errors in the document
 *
 * A declaration or an attribute that is not valid is **dropped**, as CSS says, and is a diagnostic (gtext_css_computed_diagnostic()); a lower-priority
 * declaration, the inherited value or the initial value stands. Dropping is not a failure: the call returns GTEXT_CSS_OK. A failure is a limit, memory,
 * a callback the environment or the element interface lacks (GTEXT_CSS_E_UNSUPPORTED, naming it), or a declared property the engine cannot compute.
 *
 * ## The dump and the hash
 *
 * gtext_css_computed_dump() and gtext_css_computed_hash() (css_computed.h) are the style's canonical text and its SHA-256, for the document dump of a
 * library that embeds styles.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_STYLE_H
#define GHOTI_IO_GTEXT_CSS_CSS_STYLE_H

#include <ghoti.io/text/css/css_cascade.h>
#include <ghoti.io/text/css/css_computed.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_layer.h>
#include <ghoti.io/text/css/css_match.h>
#include <ghoti.io/text/css/css_presentation.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_sheet.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct GTEXT_CSS_Style_Entry
 * @brief A list of declarations that takes part in the cascade: the body of a style rule that matched, or any other declaration list the caller has.
 */
typedef struct {
  /** The sort keys (css_cascade.h): origin, layer, context, specificity, scope, and the order of appearance (`sheet`, `rule`). `declaration` is
   *  numbered by the call, in the order the entries are given, so that equal `sheet` and `rule` keep the order of the list. `presentational_hint` and
   *  `element_attached` are the call's own and are ignored here; `important` is the declaration's. */
  GTEXT_CSS_Cascade_Entry keys;
  /** The declarations, as a `style` attribute holds them (`color: red; width: 10px !important`). */
  GTEXT_CSS_String text;
  /** Added to the offsets of its diagnostics: where the text begins in the caller's document. */
  size_t base_offset;
} GTEXT_CSS_Style_Entry;

/** @brief Where the call reads declarations from, besides its entries (a bit set). */
#define GTEXT_CSS_STYLE_PRESENTATION_ATTRIBUTES 0x1u ///< The presentation attributes of the table (css_presentation.h), through the element interface.
#define GTEXT_CSS_STYLE_STYLE_ATTRIBUTE 0x2u         ///< The `style` attribute, through the element interface.

/** @brief What a style computation may be told. */
typedef struct {
  /** Which attributes the call reads: GTEXT_CSS_STYLE_* bits. gtext_css_style_options_default() has both; a zeroed struct has neither (the entries only). Reading
   *  needs the context's element interface to have `attribute`, and for the presentation attributes `local_name` and `namespace_uri` (GTEXT_CSS_E_UNSUPPORTED
   *  naming the callback otherwise). */
  unsigned sources;
  /** The attribute-only spellings, the table and the base offset of the presentation attributes (css_presentation.h). A zeroed struct has none of the spellings;
   *  gtext_css_style_options_default() has them all. */
  GTEXT_CSS_Presentation_Options presentation;
  /** Where the `style` attribute begins in the caller's document, for its diagnostics. */
  size_t style_attribute_offset;
  /** HTML quirks (css_property.h) for the declarations. */
  bool quirks;
  /** The frozen layer registry that the layers of the entries belong to (css_layer.h), or NULL if no entry has a layer. It must outlive the call. */
  const GTEXT_CSS_Layer_Registry * layers;
  /** The limits of the candidate set (css_cascade.h); zeroed fields are the defaults. */
  GTEXT_CSS_Candidate_Options candidates;
  /** The limits of parsing a declaration list (css_core.h); zeroed fields are the defaults. */
  GTEXT_CSS_Parse_Options parse;
  /** The style rules that apply (css_sheet.h): a frozen index, and the match context to test them with (the caller's snapshot of the tree; not shared between threads). The call
   *  collects the declarations of the rules that match the element, with the keys the cascade defines, before it adds anything else, so the layers of the index's contexts
   *  are the layers of the entries it makes (`layers` serves the entries of `entries` that belong to a context the index has none of). NULL: no style rules. */
  const GTEXT_CSS_Rule_Index * rules;
  /** The match context for `rules`. Required with `rules`; must be made for the element interface of the compute context. */
  GTEXT_CSS_Selector_Match_Context * match;
  /** The pseudo-element the style is of (gtext_css_rules_collect()), or NULL for the element. */
  const GTEXT_CSS_Selector_List * pseudo;
} GTEXT_CSS_Style_Options;

/** @brief The defaults, spelled out: both attribute sources, every attribute-only spelling, no quirks. */
GTEXT_API GTEXT_CSS_Style_Options gtext_css_style_options_default(void);

/**
 * @brief Compute the style of one element.
 * @param context The context (css_computed.h). Not NULL.
 * @param element The element, as the context's element interface knows it (also passed to the environment's callbacks that take one). NULL is allowed only
 *   when the options read no attribute.
 * @param parent_style The computed style of the inheritance parent, or NULL for the root of inheritance. It must come from the same context.
 * @param entries More declaration lists, or NULL with a count of 0.
 * @param count Their number.
 * @param options NULL for gtext_css_style_options_default().
 * @param out Receives the style (one owner; release it with gtext_css_computed_free()); written only on success. Not NULL.
 * @param result NULL, or filled (css_computed.h): on a failure `status`, `message`, `property` and `name` say what and `detail` which rule was broken.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, a parent of another context, an entry the candidate set refuses, the result says which);
 *   GTEXT_CSS_E_UNSUPPORTED (a callback was needed and absent, or a declared property cannot be computed; `name` says which); GTEXT_CSS_E_OOM and the limits
 *   (GTEXT_CSS_E_LIMIT_CANDIDATES, _DECLARATIONS, _LIST, the parse limits, the limits of substitution).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_style_compute(const GTEXT_CSS_Compute_Context * context, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Computed_Style * parent_style, const GTEXT_CSS_Style_Entry * entries, size_t count, const GTEXT_CSS_Style_Options * options,
    GTEXT_CSS_Computed_Style ** out, GTEXT_CSS_Compute_Result * result);

/**
 * @brief The specified value of a property: the cascade's winner for it, with its provenance.
 * "The cascaded value" of Cascade 5 (for a logical property, the winner of its slot). A property no declaration won has none: its computed value is inherited
 * or initial. A style made by gtext_css_compute_style() from a block has no cascade and no specified values.
 * @param style The style. Not NULL.
 * @param property A property of the registry.
 * @param entry Receives the winner (css_cascade.h), valid for the life of the style; NULL if you only want to know.
 * @return true if a declaration won.
 */
GTEXT_API bool gtext_css_computed_specified(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property, GTEXT_CSS_Cascade_Entry * entry);

/** @brief gtext_css_computed_specified() for a custom property, by name (case-sensitive). */
GTEXT_API bool gtext_css_computed_specified_custom(const GTEXT_CSS_Computed_Style * style, const char * name, size_t length, GTEXT_CSS_Cascade_Entry * entry);

/** @brief The cascade's result for the style (the winners, and with them every declaration the style was made of); NULL for a style made from a block. Valid for the life of the style. */
GTEXT_API const GTEXT_CSS_Cascaded * gtext_css_computed_cascaded(const GTEXT_CSS_Computed_Style * style);

/** @brief What was dropped. */
typedef enum {
  GTEXT_CSS_STYLE_DIAG_UNKNOWN_PROPERTY = 0, ///< A declaration's property is not in the registry (or an attribute's row names one that is not).
  GTEXT_CSS_STYLE_DIAG_INVALID_VALUE,        ///< The value is not one of the property.
  GTEXT_CSS_STYLE_DIAG_UNJUDGEABLE           ///< The library cannot judge the property's value (GTEXT_CSS_E_UNSUPPORTED), so nothing was added.
} GTEXT_CSS_Style_Diagnostic_Kind;

/** @brief Where a dropped declaration was. */
typedef enum {
  GTEXT_CSS_STYLE_SOURCE_STYLE_ATTRIBUTE = 0, ///< The `style` attribute.
  GTEXT_CSS_STYLE_SOURCE_PRESENTATION,        ///< A presentation attribute.
  GTEXT_CSS_STYLE_SOURCE_ENTRY                ///< An entry the caller gave.
} GTEXT_CSS_Style_Source;

/** @brief A declaration or attribute that was dropped. */
typedef struct {
  GTEXT_CSS_Style_Diagnostic_Kind kind; ///< Why.
  GTEXT_CSS_Style_Source source;        ///< Where from.
  size_t entry;                         ///< SOURCE_ENTRY: the index of the entry.
  GTEXT_CSS_String name;                ///< The property (a declaration) or the attribute (a presentation attribute) as written; owned by the style.
  size_t offset;                        ///< The byte where it was, in the attribute or the entry's text, plus the base offset of that source.
  size_t length;                        ///< How many bytes the declaration covers (0 for a place: a presentation attribute's).
  size_t line;                          ///< 1-based line of `offset` in the declaration list; 0 for a presentation attribute.
  size_t column;                        ///< Its column in UTF-16 code units, 1-based; 0 for a presentation attribute.
} GTEXT_CSS_Style_Diagnostic;

/** @brief How many declarations and attributes were dropped (all of them, though the style keeps only the first 1,024, or the first `candidates.max_diagnostics` of the options). NULL is 0. */
GTEXT_API size_t gtext_css_computed_diagnostic_total(const GTEXT_CSS_Computed_Style * style);

/** @brief The `index`th kept diagnostic, in the order of the drops (presentation attributes, the `style` attribute, then the entries); false past the end. */
GTEXT_API bool gtext_css_computed_diagnostic(const GTEXT_CSS_Computed_Style * style, size_t index, GTEXT_CSS_Style_Diagnostic * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_STYLE_H
