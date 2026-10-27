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
 * @file css_cascade.h
 * @brief The cascade: from the candidate declarations of one element, with the keys that sort them, to the declaration that wins each property.
 *
 * Reference: CSS Cascading and Inheritance Level 5, sections 6.1 "Cascade Sorting Order", 6.5 "Precedence of Non-CSS Presentational Hints",
 * 7.3.4 to 7.3.6 (`revert`, `revert-layer`, `revert-rule`) and 7.4 (`all`) (Editor's Draft of 13 April 2026), and Level 6, section 3.1 (Scope
 * Proximity, Editor's Draft of 28 April 2026), both served 2026-10-26. The sentences the tests quote are in tests/test-css-cascade.cpp.
 *
 * ## What is and is not here
 *
 * The cascade is a **declaration-source interface**. A caller supplies the candidate declarations of one element with their sort keys: the selector
 * matcher (C3) will supply the declarations of the style rules that match and their specificity, the attribute readers (C2d) the `style` attribute and the
 * presentation attributes, and a test builds entries directly. Nothing here parses or matches a selector, or evaluates a media, supports or container
 * condition (C4 supplies the filter that says which `@layer` rules count: css_layer.h), or substitutes `var()` (C2c): a pending value is carried, not read.
 *
 * ## The sort order
 *
 * The criteria, in descending order of precedence, are the specification's, and each has a field of GTEXT_CSS_Cascade_Entry:
 *
 * 1. **Origin and importance**: "Transition declarations; Important user agent declarations; Important user declarations; Important author declarations;
 *    Animation declarations; Normal author declarations; Normal user declarations; Normal user agent declarations. Declarations from origins earlier in
 *    this list win over declarations from later origins."
 * 2. **Context**: "for normal rules the declaration from the outer context wins, and for important rules the declaration from the inner context wins", the
 *    contexts nested "in shadow-including tree order". `context` is that order: 0 is the outermost, and a larger number is further in.
 * 3. **Element-attached styles**: "Separately for normal and important declarations, declarations that are attached directly to an element (such as the
 *    contents of a style attribute) rather than indirectly mapped by means of a style rule selector take precedence over declarations the same importance
 *    that are mapped via style rule."
 * 4. **Layers**: "for normal rules the declaration whose cascade layer is latest in the layer order wins, and for important rules the declaration whose
 *    cascade layer is earliest wins", the unlayered declarations being "added to an implicit final layer" (css_layer.h).
 * 5. **Specificity**: "the declaration belonging to the style rule with the highest specificity wins".
 * 6. **Scope proximity**: "the declaration with the fewest generational or sibling-element hops between the scoping root and the scoped style rule subject
 *    wins. For this purpose, style rules without a scoping root are considered to have infinite proximity hops."
 * 7. **Order of appearance**: "The last declaration in document order wins." The order is (`sheet`, `rule`, `declaration`), each from 0 and compared in that
 *    order; two entries that are equal even then (which a caller has no reason to make) are ordered by the order they were added.
 *
 * Cascade 6 states proximity in prose as applied "regardless of specificity or order of appearance" and lists it after specificity in the sorting order;
 * this follows the list (the sorting criteria are what the section defines, and the prose describes proximity's own effect among rules that tie
 * on specificity). Presentational hints are "author-level style at the start of the author style sheet with specificity zero": the entry flag puts a hint
 * below every other normal author declaration of its context, layered or not, and above the user and user-agent origins.
 *
 * ## Rolling back
 *
 * `revert`, `revert-layer` and `revert-rule` are resolved here, so the computed-value layer (css_computed.h) never sees one. `revert` "rolls back the cascade to
 * the cascaded value of the earlier origin": from an author declaration (the animation and transition origins included) as if no author-origin declaration were
 * specified, from a user declaration as if no author or user declaration were, and from a user-agent declaration it is `unset`. `revert-layer` rolls back
 * "as if no rules were specified in the current cascade layer, or between its normal and important levels in the cascade"; "for revert-layer in important
 * element-attached styles it only reverts the element-attached styles and the intervening animation origin"; the animation origin "forms its own cascade
 * layer". `revert-rule` rolls back "as if the current style rule had not been present at all". Where nothing is left, the property has **no declared value**
 * and takes the inherited or initial value (`unset`): the property is simply absent from the result.
 *
 * ## `all`, shorthands, logical groups
 *
 * A shorthand is expanded through css_shorthand.h and each longhand carries the declaration's keys; a shorthand holding `var()` stays pending longhands. `all`
 * is one candidate that applies to every property but `direction`, `unicode-bidi` and the custom properties, not one candidate for each. The members of a
 * logical property group that share a physical slot in the element's flow "are cascaded together as one" (Logical Properties 1, section 4): the candidates of
 * `margin-left` and `margin-inline-start` are one set when `margin-inline-start` is the left, and the winner is one of them. The flow is the element's computed
 * `writing-mode`, `direction` and `text-orientation`, which are cascaded first: from the cascade of those three properties, with `inherit` and `unset`
 * taken from the inheritance parent's flow (GTEXT_CSS_Cascade_Options::parent_flow), or given outright (GTEXT_CSS_Cascade_Options::flow). A flow property that is
 * still pending is taken as `unset`, which is what an invalid-at-computed-value-time declaration is; a caller that substitutes `var()` cascades again with the flow.
 *
 * ## What comes out
 *
 * One winning declaration for each property (and each custom property name) that has one, with its provenance, in a canonical order (property id, then the custom
 * properties by name) that does not depend on the order the candidates were added in. gtext_css_cascaded_to_block() makes the declaration block that
 * gtext_css_compute_style() takes. On request the candidates that did not win are kept, in order, with the reason each lost.
 *
 * ## Cost, limits, reentrancy
 *
 * A property that does not roll back is decided in one pass over its candidates. Only a property whose winner is a rolling keyword sorts its candidates
 * (O(k log k) for k of them); `all` is merged, not repeated. The one other sort is of the names of the custom properties that have a winner, so that the
 * output order is canonical. The number of candidates is bounded (GTEXT_CSS_E_LIMIT_CANDIDATES, with the declaration that crossed it), and nothing recurses.
 * A candidate set is mutable until the first cascade seals it; afterwards it and every result are immutable and may be read by any number of threads, and each
 * cascade owns its state.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CASCADE_H
#define GHOTI_IO_GTEXT_CSS_CSS_CASCADE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_layer.h>
#include <ghoti.io/text/css/css_logical.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/css/css_shorthand.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- specificity ---------------------------------------------------------------------------------------------- */

/** @brief A specificity (A, B, C) as Selectors 4 defines it: the counts of id selectors, of class, attribute and pseudo-class selectors, and of type and pseudo-element selectors. */
typedef struct {
  uint32_t a; ///< Id selectors.
  uint32_t b; ///< Class, attribute and pseudo-class selectors.
  uint32_t c; ///< Type and pseudo-element selectors.
} GTEXT_CSS_Specificity;

/**
 * @brief A specificity from counts of any size. **Decided:** a component saturates at `UINT32_MAX`, it does not wrap: a selector with 2^32 ids is not weaker than one with none.
 */
GTEXT_API GTEXT_CSS_Specificity gtext_css_specificity_make(uint64_t a, uint64_t b, uint64_t c);

/** @brief The sum of two specificities (a selector list's components added together, `:is()` and `:not()` taking the argument's), each component saturating at `UINT32_MAX`. */
GTEXT_API GTEXT_CSS_Specificity gtext_css_specificity_add(GTEXT_CSS_Specificity x, GTEXT_CSS_Specificity y);

/** @brief Compare lexicographically: -1, 0 or 1 as `x` is less than, equal to or greater than `y`. */
GTEXT_API int gtext_css_specificity_compare(GTEXT_CSS_Specificity x, GTEXT_CSS_Specificity y);

/* ---- entries -------------------------------------------------------------------------------------------------- */

/**
 * @struct GTEXT_CSS_Cascade_Entry
 * @brief A candidate declaration: its sort keys (set by the caller) and, when the library returns one, what it declares.
 *
 * Passed to gtext_css_candidates_add() only the keys are read; the value comes from the parse result. An entry the library returns has both.
 */
typedef struct {
  /* ---- the keys, from the caller ---- */
  GTEXT_CSS_Origin origin;             ///< The origin. A zeroed entry is a normal user-agent declaration.
  bool important;                      ///< Declared `!important`. Not for the animation and transition origins (GTEXT_CSS_E_INVALID).
  bool presentational_hint;            ///< A non-CSS presentational hint or SVG presentation attribute: author origin, never important, unlayered, not element-attached; its specificity is taken as zero.
  bool element_attached;               ///< Attached to the element (a `style` attribute): unlayered; its specificity and scope are not used.
  GTEXT_CSS_Layer_Id layer;            ///< The layer of the origin in the layer registry (GTEXT_CSS_LAYER_NONE: unlayered).
  uint32_t context;                    ///< The shadow-tree context, 0 for the outermost (see the file's description).
  bool scoped;                         ///< The rule is a scoped style rule (in an `@scope`); false: no scoping root, "infinite proximity hops".
  uint32_t scope_distance;             ///< `scoped`: the generational or sibling-element hops from the scoping root to the rule's subject (0 is the root itself).
  GTEXT_CSS_Specificity specificity;   ///< The specificity of the style rule (of the selector of its list that matched).
  uint32_t sheet;                      ///< Order of appearance: the style sheet, in final style sheet order.
  uint32_t rule;                       ///< ... the rule in it, in document order.
  uint32_t declaration;                ///< ... the declaration in the rule.
  GTEXT_CSS_Id rule_id;                ///< Provenance: the C0 id of the rule (0 for none).
  GTEXT_CSS_Id declaration_id;         ///< Provenance: the C0 id of the declaration (0 for none).
  GTEXT_CSS_Span span;                 ///< Provenance: where the declaration is in its source.
  /* ---- what it declares, from the library ---- */
  GTEXT_CSS_Property_Id property;      ///< The property it sets: a longhand; for a declaration of `all` that won a property, that property; the custom property pattern for a custom one.
  GTEXT_CSS_String custom_name;        ///< A custom property's name as written; else empty.
  GTEXT_CSS_Property_Outcome outcome;  ///< VALID, WIDE or PENDING.
  GTEXT_CSS_Wide_Keyword wide;         ///< WIDE: which keyword.
  const GTEXT_CSS_Value_Set * set;     ///< VALID, PENDING: the value, owned by the candidate set or the result that returned the entry.
  GTEXT_CSS_Property_Id shorthand;     ///< The shorthand it was declared through, or 0.
  size_t group;                        ///< The declaration it came from: equal for the longhands of one shorthand, in the order the declarations were added.
  bool from_all;                       ///< Declared by `all`.
  size_t index;                        ///< Its position in the candidate set, in the order added.
} GTEXT_CSS_Cascade_Entry;

/** @brief What a candidate or cascade call reports. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;        ///< What the call returned.
  const char * message;           ///< A static description; never NULL after a call.
  GTEXT_CSS_Property_Id property; ///< The property concerned, or 0.
  char name[96];                  ///< The name concerned (a property, a custom property, a callback), cut to fit.
  const char * detail;            ///< A static sentence saying which rule was broken, or NULL.
  GTEXT_CSS_Id declaration;       ///< The C0 id of the declaration concerned (a limit reached at it), or 0.
  GTEXT_CSS_Span span;            ///< Where that declaration is.
  size_t work;                    ///< Units of work done (candidates and steps visited, comparisons made), for the quadratic suite.
} GTEXT_CSS_Cascade_Result;

/* ---- the candidate set ---------------------------------------------------------------------------------------- */

/** @brief The candidate declarations of one element (the struct is private). Reference counted. */
typedef struct GTEXT_CSS_Candidates GTEXT_CSS_Candidates;

/** @brief The allocator and limits of a candidate set. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the registry's.
  size_t max_candidates;              ///< Most candidates (a shorthand counts its longhands). Default 1,048,576; at most 2^28. One more is GTEXT_CSS_E_LIMIT_CANDIDATES.
  size_t max_diagnostics;             ///< Most diagnostics kept (more are counted). Default 1,024; at most 2^20.
} GTEXT_CSS_Candidate_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Candidate_Options gtext_css_candidate_options_default(void);

/**
 * @brief Make an empty candidate set.
 *
 * @param registry The registry. Not NULL; it must outlive the set and every result.
 * @param layers A **frozen** layer registry (css_layer.h) that the entries' layers belong to; NULL if no entry has a layer (a layer is then GTEXT_CSS_E_INVALID).
 *   It must outlive the set.
 * @param options NULL for the defaults.
 * @param out Receives the set; written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, or a layer registry that is not frozen); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_candidates_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Layer_Registry * layers,
    const GTEXT_CSS_Candidate_Options * options, GTEXT_CSS_Candidates ** out);

/** @brief Another owner of the set (it is freed when the last lets go). */
GTEXT_API GTEXT_CSS_Candidates * gtext_css_candidates_retain(GTEXT_CSS_Candidates * set);

/** @brief One owner fewer; the last frees the set and releases its values. NULL is ignored. */
GTEXT_API void gtext_css_candidates_free(GTEXT_CSS_Candidates * set);

/**
 * @brief Add a declaration's value with its keys.
 *
 * A longhand adds one candidate; a shorthand its expansion (all-or-nothing: on a failure the set is as it was), each longhand with the keys; `all` one candidate; a
 * CSS-wide keyword or a pending value a candidate of that kind. The value's set is retained. `value->property` must be a property of the registry.
 *
 * @param set The set. Not NULL; not sealed.
 * @param keys The keys. Not NULL; the value fields are ignored.
 * @param value What gtext_css_parse_property() returned: VALID, WIDE or PENDING. A custom property is added with gtext_css_candidates_add_custom().
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument; a result that is INVALID or UNKNOWN; a custom property; keys that contradict themselves, or a layer
 *   the layer registry lacks, or a set that is sealed: the result's `detail` says which); GTEXT_CSS_E_UNSUPPORTED (a shorthand with no rule: the result names it);
 *   GTEXT_CSS_E_LIMIT_CANDIDATES (at this declaration); GTEXT_CSS_E_OOM; and the statuses of expansion.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_candidates_add(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys,
    const GTEXT_CSS_Property_Value * value, GTEXT_CSS_Cascade_Result * result);

/** @brief Add a custom property's declaration (VALID or WIDE; case of the name is kept). @return As gtext_css_candidates_add(). */
GTEXT_API GTEXT_CSS_Status gtext_css_candidates_add_custom(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys, const char * name,
    size_t name_length, const GTEXT_CSS_Property_Value * value, GTEXT_CSS_Cascade_Result * result);

/** @brief What was dropped, and why. */
typedef enum {
  GTEXT_CSS_DIAG_UNKNOWN_PROPERTY = 0, ///< The property is not in the registry: the declaration is dropped.
  GTEXT_CSS_DIAG_INVALID_VALUE         ///< The value is not one of the property: the declaration is dropped and a lower-priority valid one wins.
} GTEXT_CSS_Diagnostic_Kind;

/** @brief A declaration the cascade dropped. */
typedef struct {
  GTEXT_CSS_Diagnostic_Kind kind;  ///< Why.
  GTEXT_CSS_String name;           ///< The property's name as written; owned by the set.
  GTEXT_CSS_Id declaration;        ///< The C0 id of the declaration.
  GTEXT_CSS_Span span;             ///< Where it is.
} GTEXT_CSS_Cascade_Diagnostic;

/**
 * @brief Parse a declaration of a C0 tree and add it if it is valid.
 *
 * "An invalid declaration (a failed parse) never enters the cascade, so a lower-priority valid declaration wins, as the draft says; an unknown property is
 * dropped with a diagnostic naming it." The importance, the declaration id and the span are the declaration's own, whatever `keys` holds.
 *
 * @param set The set. Not NULL.
 * @param keys The keys. Not NULL.
 * @param declaration The declaration. Not NULL.
 * @param options The property parse options (css_property.h), or NULL.
 * @param outcome Receives what the parse came to (UNKNOWN and INVALID add nothing); NULL if not wanted.
 * @param result NULL, or filled.
 * @return As gtext_css_candidates_add(), and the statuses of gtext_css_parse_property_declaration() (GTEXT_CSS_E_UNSUPPORTED for a property whose value cannot be
 *   judged: the declaration is not added and the caller decides).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_candidates_add_declaration(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys,
    const GTEXT_CSS_Declaration * declaration, const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Outcome * outcome,
    GTEXT_CSS_Cascade_Result * result);

/**
 * @brief Parse a declaration given as text and add it if it is valid (`important` is in `keys`).
 *
 * @return As gtext_css_candidates_add_declaration().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_candidates_add_text(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys, const char * name,
    size_t name_length, const char * text, size_t text_length, const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Outcome * outcome,
    GTEXT_CSS_Cascade_Result * result);

/** @brief How many candidates the set holds. NULL is 0. */
GTEXT_API size_t gtext_css_candidates_count(const GTEXT_CSS_Candidates * set);

/** @brief The `index`th candidate, in the order added; false past the end. Its pointers are owned by the set. */
GTEXT_API bool gtext_css_candidates_entry(const GTEXT_CSS_Candidates * set, size_t index, GTEXT_CSS_Cascade_Entry * out);

/** @brief How many declarations were dropped (all of them, though only `max_diagnostics` are kept). NULL is 0. */
GTEXT_API size_t gtext_css_candidates_diagnostic_total(const GTEXT_CSS_Candidates * set);

/** @brief The `index`th kept diagnostic, in the order of the drops; false past the end. */
GTEXT_API bool gtext_css_candidates_diagnostic(const GTEXT_CSS_Candidates * set, size_t index, GTEXT_CSS_Cascade_Diagnostic * out);

/** @brief Whether a cascade has sealed the set (no more candidates are added). */
GTEXT_API bool gtext_css_candidates_sealed(const GTEXT_CSS_Candidates * set);

/* ---- cascading ------------------------------------------------------------------------------------------------ */

/** @brief What cascading may be told. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the registry's.
  /** The inheritance parent's flow (gtext_css_computed_flow()), for `inherit` and `unset` of `writing-mode`, `direction` and `text-orientation`; NULL for the root (horizontal-tb, ltr, mixed). */
  const GTEXT_CSS_Flow * parent_flow;
  /** This element's flow, when the caller knows it (the flow is then not cascaded): NULL to cascade it. */
  const GTEXT_CSS_Flow * flow;
  /** Keys the hash of the custom property names (no names an author chooses may defeat it). 0: a seed drawn from the addresses of the run. A fixed seed makes a run reproducible, not safer. */
  uint64_t hash_seed;
  /** Keep the candidates that did not win, with the reason (gtext_css_cascaded_loser()). Costs a sort of each property's candidates when asked. */
  bool keep_losers;
} GTEXT_CSS_Cascade_Options;

/** @brief The winners of a cascade (the struct is private). Reference counted: a result holds the candidate set it was made from. */
typedef struct GTEXT_CSS_Cascaded GTEXT_CSS_Cascaded;

/**
 * @brief Cascade the candidates of one element.
 *
 * Seals the set. The result is immutable and independent of the order the candidates were added in, except where two candidates are equal in every key.
 *
 * @param set The candidates. Not NULL.
 * @param options NULL for the defaults.
 * @param out Receives the result; written only on success. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument); GTEXT_CSS_E_OOM; GTEXT_CSS_E_INTERNAL (a defect).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_cascade(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Options * options, GTEXT_CSS_Cascaded ** out,
    GTEXT_CSS_Cascade_Result * result);

/** @brief Free a result; NULL is ignored. */
GTEXT_API void gtext_css_cascaded_free(GTEXT_CSS_Cascaded * cascaded);

/** @brief The element's flow (the options' `flow`, or the cascaded one). */
GTEXT_API GTEXT_CSS_Flow gtext_css_cascaded_flow(const GTEXT_CSS_Cascaded * cascaded);

/** @brief How many properties have a winning declaration (a logical group's slot counts once). NULL is 0. */
GTEXT_API size_t gtext_css_cascaded_count(const GTEXT_CSS_Cascaded * cascaded);

/**
 * @brief The `index`th winner in the canonical order: the registered properties by id, then the custom properties by name.
 *
 * @param entry Receives it, with the winning declaration's keys and value; its pointers are owned by the result. Not NULL.
 * @param rollbacks Receives how many `revert`, `revert-layer` and `revert-rule` declarations were rolled back to reach it (NULL if not wanted).
 * @return true, or false past the end.
 */
GTEXT_API bool gtext_css_cascaded_at(const GTEXT_CSS_Cascaded * cascaded, size_t index, GTEXT_CSS_Cascade_Entry * entry, size_t * rollbacks);

/**
 * @brief The winner for a property, or for a custom property by name.
 *
 * A member of a logical group finds the winner of its slot, if its own declaration or a member's that maps to the same slot won it: `margin-left` finds
 * `margin-inline-start`'s declaration where that won the left side.
 *
 * @param property A property of the registry, or GTEXT_CSS_PROPERTY_NONE with a `custom_name`.
 * @param custom_name A custom property's name (case kept), or NULL.
 * @param custom_length Its length.
 * @param entry Receives the winner; NULL if not wanted.
 * @return true if the property has a winner.
 */
GTEXT_API bool gtext_css_cascaded_find(const GTEXT_CSS_Cascaded * cascaded, GTEXT_CSS_Property_Id property, const char * custom_name,
    size_t custom_length, GTEXT_CSS_Cascade_Entry * entry);

/** @brief Why a candidate did not win. */
typedef enum {
  GTEXT_CSS_LOSS_OUTRANKED = 0, ///< It sorts below the winner.
  GTEXT_CSS_LOSS_ROLLED_BACK    ///< It sorts above the winner and was rolled back by a `revert`, `revert-layer` or `revert-rule` (or was one).
} GTEXT_CSS_Loss_Reason;

/**
 * @brief How many candidates lost to the `index`th winner (those of the same property, or logical slot, and `all`). 0 unless keep_losers was set.
 */
GTEXT_API size_t gtext_css_cascaded_loser_count(const GTEXT_CSS_Cascaded * cascaded, size_t index);

/**
 * @brief The `loser`th candidate that lost to the `index`th winner, in descending order of precedence ("who was overridden").
 *
 * @param reason Receives why (NULL if not wanted).
 * @return true, or false past the end (or without keep_losers).
 */
GTEXT_API bool gtext_css_cascaded_loser(const GTEXT_CSS_Cascaded * cascaded, size_t index, size_t loser, GTEXT_CSS_Cascade_Entry * entry,
    GTEXT_CSS_Loss_Reason * reason);

/**
 * @brief Write the result in a canonical, versioned text: a first line `cascaded 1`, then one line for each winner in the canonical order (property, value
 *   as the declaration wrote it, `!important`, and in brackets the origin, layer, specificity, scope, order and rollbacks). Equal results give equal text.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_WRITE.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_cascaded_dump(const GTEXT_CSS_Cascaded * cascaded, const GTEXT_Markup_Sink * sink);

/** @brief The 64-bit FNV-1a hash of the text gtext_css_cascaded_dump() writes. @return OK, E_INVALID or E_OOM. */
GTEXT_API GTEXT_CSS_Status gtext_css_cascaded_hash(const GTEXT_CSS_Cascaded * cascaded, uint64_t * hash);

/**
 * @brief Make the declaration block of the winners, for gtext_css_compute_style(): each winner once, `all`'s expanded to the properties it won, shorthands
 *   and logical groups already resolved. A property absent from the block takes its inherited or initial value.
 *
 * @param cascaded The result. Not NULL.
 * @param options The block's options (css_shorthand.h), or NULL.
 * @param out Receives the block; written only on success. Free it with gtext_css_block_free(). Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_DECLARATIONS.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_cascaded_to_block(const GTEXT_CSS_Cascaded * cascaded, const GTEXT_CSS_Declaration_Options * options,
    GTEXT_CSS_Declaration_Block ** out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CASCADE_H
