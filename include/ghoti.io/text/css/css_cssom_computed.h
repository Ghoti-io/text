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
#include <ghoti.io/text/css/css_custom_function.h>
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
  GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT,  ///< The category asks for the used value, layout was not asked (no `used_value` callback) and the computed value stands in.
  GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE ///< An animation of the element targets the property and the layout gave it no time (no `animation_time` callback, or `*known` false): the underlying value stands in.
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
/**
 * @brief The *after-change style* of an element or one of its pseudo-elements (CSS Transitions 1, "Starting of transitions"): its computed style as of now, computed **without** transitions
 * and inheriting from the after-change style of its parent, for gtext_css_transitions_update().
 *
 * The chain from the root is computed again for every call and nothing is kept (the cache holds the styles a read shows, transitions included). The animations are sampled as the layout
 * interface gives them.
 *
 * @param document The document. @param element The element (the tree's own handle). @param pseudo The pseudo-element as `getComputedStyle()` takes it (`"::marker"`), or NULL.
 * @param pseudo_length Its length. @param out Receives a style the caller releases with gtext_css_computed_free(); NULL (with GTEXT_CSS_OK) when `pseudo` is not a pseudo-element selector or the
 * element is not rendered, the cases in which `getComputedStyle()` answers an empty declaration. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED (the document has no element interface); GTEXT_CSS_E_OOM; any status the tree's callbacks fail with.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_after_change_style(GTEXT_CSSOM_Document * document, const void * element, const char * pseudo, size_t pseudo_length,
    GTEXT_CSS_Computed_Style ** out);

/** @brief The transitions of the elements of a document: a set (css_transition.h) and the before-change style for each element and pseudo-element that has been through a style change event. */
typedef struct GTEXT_CSSOM_Transition_Tracker GTEXT_CSSOM_Transition_Tracker;

/**
 * @brief Make a tracker for a document: the reference consumer of gtext_css_transitions_update() and gtext_cssom_document_after_change_style().
 *
 * The specification makes the implementation keep the running and the completed transitions of each element; the library keeps nothing between calls, so a consumer that wants
 * `getComputedStyle()` to show transitions holds them in a tracker (or in a structure of its own, with the same calls). The tracker is not shared between threads, and it must be freed
 * before the document is.
 *
 * Use: install `gtext_cssom_transition_tracker_lookup()` and `..._version()` from the layout interface's `transitions` and `transition_version`; call
 * gtext_cssom_transition_tracker_flush() when a style change event may have happened for an element (a read that wants an up-to-date style is one, as in browsers); set the time first.
 *
 * @param document The document. @param allocator NULL for the default. @param out Receives the tracker. @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID, GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_transition_tracker_new(GTEXT_CSSOM_Document * document, const GTEXT_Allocator * allocator, GTEXT_CSSOM_Transition_Tracker ** out);

/** @brief Release a tracker and every set it holds; NULL is ignored. */
GTEXT_API void gtext_cssom_transition_tracker_free(GTEXT_CSSOM_Transition_Tracker * tracker);

/** @brief The time of the next style change events and of the current values the layout reports, in milliseconds. It changes the version when it changes. */
GTEXT_API void gtext_cssom_transition_tracker_set_time(GTEXT_CSSOM_Transition_Tracker * tracker, double time_ms);

/**
 * @brief A style change event for an element and its pseudo-element.
 *
 * The elements from the root down to `element` are brought up to date first (the after-change style of an element inherits from its parent's, and a transition of an inherited property on
 * a parent changes what the child inherits): for each, the after-change style is computed (gtext_cssom_document_after_change_style()), the element's transitions are updated against the style
 * of the previous event (gtext_css_transitions_update(); the first event an element is in has no before-change style, so starts nothing) and the after-change style is kept for the next event.
 * Then the same for the pseudo-element `pseudo` of the element, if there is one. An element that is not rendered, or a pseudo-element that is not a valid one, is left alone.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_transition_tracker_flush(GTEXT_CSSOM_Transition_Tracker * tracker, const void * element, const char * pseudo, size_t pseudo_length);

/** @brief Forget an element and its pseudo-elements ("If an element is no longer in the document, implementations must cancel any running transitions on it"). @return How many entries went. */
GTEXT_API size_t gtext_cssom_transition_tracker_forget(GTEXT_CSSOM_Transition_Tracker * tracker, const void * element);

/** @brief For GTEXT_CSSOM_Layout_Interface::transitions: the set of an element or pseudo-element and the tracker's time. `*set` is NULL for one with no running transition. */
GTEXT_API GTEXT_CSS_Status gtext_cssom_transition_tracker_lookup(GTEXT_CSSOM_Transition_Tracker * tracker, const void * element, const GTEXT_CSS_Selector_List * pseudo,
    const GTEXT_CSS_Transition_Set ** set, double * time_ms);

/** @brief For GTEXT_CSSOM_Layout_Interface::transition_version. */
GTEXT_API uint64_t gtext_cssom_transition_tracker_version(const GTEXT_CSSOM_Transition_Tracker * tracker);

GTEXT_API GTEXT_CSS_Status gtext_cssom_computed_is_empty(GTEXT_CSSOM_Decl * decl, bool * empty);

/**
 * @brief Where the resolved value of a property of a computed declaration came from. A shorthand is USED if any longhand is, else ABSENT_LAYOUT or ABSENT_TIMELINE (the first it meets) if any is, else COMPUTED.
 * @param decl A computed declaration. Not NULL.
 * @param name The property (a longhand, a shorthand, or a custom property, which is COMPUTED unless an animation targets it and has no time).
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

/**
 * @brief The part of a container interface (css_container.h) that a document can answer: fills `size`, `user` (the document) and `computed_style`, and `compute_context` with the
 *   one the document computes in: the one it was given (gtext_cssom_document_set_compute_context(), GTEXT_CSSOM_Config::compute_context), else the one it makes for itself. Every other callback is left NULL for the caller to set: a
 *   container's box, scroll state and anchored fallback are facts about a layout.
 *
 * Pass the result as GTEXT_CSS_Conditions_Options::container of the conditions object the document evaluates with, then give the document that object
 * (gtext_cssom_document_set_conditions()); a `@container` rule, a container query length and `if(style())` are then answered from the styles the document computes. The conditions object
 * copies the struct, so `compute_context` is the one at the time of the call: make the interface again (and give the document the new conditions object) after replacing the compute context.
 *
 * `computed_style` returns **the style the document has already computed** for the element in the state of the tree it is computing, and NULL for one it has not (the library asks
 * for the ancestors of the element it is styling, which the chain of a read computes first) or no longer holds (the cache is bounded, GTEXT_CSSOM_Config::max_cached_styles).
 * The pointer is borrowed: it is valid until the read that asked ends.
 *
 * @param document The document. Not NULL; it must outlive every conditions object that holds the interface.
 * @param out Receives the interface. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM (the document could not make its compute context).
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_container_interface(GTEXT_CSSOM_Document * document, GTEXT_CSS_Container_Interface * out);

/**
 * @brief The registrations that the `@property` rules of the document's style sheets make.
 *
 * "If multiple valid @property rules are defined for the same name, the last one wins" (CSS Properties and Values API 1): the rules of the sheets that take part (not disabled, their
 * media true for the conditions), in the order the cascade reads the sheets, entering `@layer` blocks and the `@media`, `@supports` and `@container` rules whose condition holds. A rule
 * that is not valid is not in the sheet at all (the parse of a sheet drops it, as the browsers do) and registers nothing; the diagnostics the set reports are of the valid rules that another
 * replaced or a caller's registration overrode (css_at_property.h). A name the registry has from a caller's registration is not registered by a rule (the same
 * sentence: "A custom property registration from CSS.registerProperty() further wins over any @property rules").
 *
 * **What it is applied to.** The compute context the document makes for itself is made with this set, so `getComputedStyle()` reads a registered property as its computed value (a
 * `calc()` resolved, a font-relative length absolute, an unset non-inherited property as its initial value) and `if(style())` compares computed values. It is rebuilt when the set
 * changes, which happens when a sheet with such a rule is inserted, removed, disabled or has its rules edited, or the conditions change what a conditional rule holds; a document
 * with no `@property` rule does nothing for them. A compute context the caller supplies (GTEXT_CSSOM_Config::compute_context) keeps its own registrations: the rules are reported here
 * and `*applied` is false (the counter `property_unapplied` says how many registrations were not applied).
 *
 * **Not read.** The sheets registered for the user-agent and user origins (gtext_cssom_document_register_sheet()) are compiled sheets and have no rules to read; the sheets an `@import`
 * rule loaded are not entered; and the order of the layers is not applied to the order of the rules (the rules are in the order of the sheet).
 *
 * **Lifetime.** The set is the document's: valid until the next call that makes the index current (a read of a computed declaration, gtext_cssom_document_index()) after the sheets changed. A
 * conditions object given the result of gtext_cssom_document_container_interface() holds the compute context at that time, which stays valid, with its registrations, until the
 * document goes; make the interface again after the set changes to see the new ones.
 *
 * @param document The document. Not NULL.
 * @param set Receives the set, or NULL when the rules make no registration and raise no diagnostic (a sheet of style rules alone is not even read). Not NULL.
 * @param applied Receives whether the document's computed styles use it; NULL if not wanted.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; the limits of css_at_property.h (GTEXT_CSS_E_LIMIT_REGISTRY ...) when the rules are more than it holds, and the statuses of building the index.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_property_rules(GTEXT_CSSOM_Document * document, const GTEXT_CSS_At_Property_Set ** set, bool * applied);

/**
 * @brief The custom functions that the `@function` rules of the document's style sheets define.
 *
 * What gtext_cssom_document_property_rules() is for `@property`: the set the document's compute context is made with, so that `getComputedStyle()` evaluates `--name(...)` where a
 * custom property takes a value. "If more than one @function exists for a given name, then the rule in the stronger cascade layer wins, and rules defined later win within the same
 * layer" (CSS Functions and Mixins 1, "Defining Custom Functions"); the rules are those of the sheets that take part (not disabled, media true for the conditions), in the order the cascade
 * reads the sheets, entering `@layer` blocks and the `@media`, `@supports` and `@container` rules whose condition holds. Inspect the set with gtext_css_function_count(),
 * gtext_css_function_at(), gtext_css_function_find() and gtext_css_function_parameter(), and gtext_css_function_diagnostic_total() and gtext_css_function_diagnostic() for the rules that were not valid.
 * The set keeps copies of the rules (`own_rules`), so what it returns for a function stays valid for the set's life.
 *
 * **What it is applied to.** The compute context the document makes for itself is made with this set; it is made again when the set changes (a sheet with such a rule inserted, removed,
 * disabled, or its rules edited, or the conditions changing what a conditional rule holds). A compute context the caller supplies (GTEXT_CSSOM_Config::compute_context) keeps its own
 * functions: the rules are reported here and `*applied` is false.
 *
 * **Not read.** As for `@property`: the sheets registered for the user-agent and user origins are compiled and have no rules to read, and the sheets an `@import` rule loaded are not entered.
 *
 * **Lifetime.** The set is the document's: valid until the next call that makes the index current after the sheets changed.
 *
 * @param document The document. Not NULL.
 * @param set Receives the set, or NULL when no rule defines a function and none raised a diagnostic (a sheet of style rules alone is not even read). Not NULL.
 * @param applied Receives whether the document's computed styles use it; NULL if not wanted.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; the limits of css_custom_function.h (GTEXT_CSS_E_LIMIT_REGISTRY ...) when the rules are more than it holds, and the statuses of building the index.
 */
GTEXT_API GTEXT_CSS_Status gtext_cssom_document_function_rules(GTEXT_CSSOM_Document * document, const GTEXT_CSS_Function_Set ** set, bool * applied);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CSSOM_COMPUTED_H
