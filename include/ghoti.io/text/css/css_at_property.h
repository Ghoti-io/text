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
 * @file css_at_property.h
 * @brief `@property` rules: reading them from stylesheets into the registrations a style is computed with.
 *
 * Reference: the Editor's Draft of CSS Properties and Values API Level 1 (css-houdini-drafts 27d37c6, 31 October 2026), 2 ("Registered Custom Properties"), 3 ("The `@property`
 * Rule"), 4.1 and 5 ("Syntax Strings"); CSS Cascade Level 5 for what a conditional rule does to the rules inside it; CSS Functions and Mixins 1 for `<css-type>`.
 *
 * ## What a rule has to say
 *
 * "While the <custom-property-name> is required, all of the descriptors are optional. When omitted, it matches the behavior of an unregistered custom property": a rule says as much or
 * as little as it likes. The initial values of the descriptors are `*` (`syntax`), `true` (`inherits`) and "the guaranteed-invalid value" (`initial-value`).
 *
 * - **The prelude** is `<custom-property-name>#`: `@property --a, --b { ... }` registers both names, each with the same descriptors ("a custom property registration for each
 *   <custom-property-name> in the rule's prelude ... and all other fields identical"); the later of two rules and a caller's registration win for each name on its own. A prelude that is
 *   not a list of dashed identifiers other than `--` is not a prelude of the rule, and the rule is ignored (GTEXT_CSS_AT_PROP_BAD_PRELUDE).
 * - **The statement form**, `@property <custom-property-name> <css-type>? [: <declaration-value>? ]? ;`, "is exactly equivalent to the normal rule form": the `<css-type>` (one syntax component,
 *   or `type(<syntax>)`) is the `syntax`, the value after the colon the `initial-value` (the colon decides whether there is one, and the value may be empty), and "The other descriptors of
 *   the @property rule are always treated as omitted", so the property inherits. It has one name.
 * - **A descriptor that is not valid is ignored**, and the rule stands: a `syntax` that is no syntax string ("the descriptor is invalid and must be ignored"), an `inherits` that is not
 *   `true` or `false`, an `initial-value` that does not "successfully parse according to the rule's syntax descriptor", a name the rule does not know ("Unknown descriptors are invalid and
 *   ignored, but do not invalidate the @property rule"), a descriptor with `!important`. Each is reported (GTEXT_CSS_At_Property_Diagnostic).
 * - **An initial value that parses and needs the element** (`3em`, `currentcolor`, `var(--x)`) is a registration "register a custom property" refuses ("If parsed initial value is not
 *   computationally independent, throw a SyntaxError"), and the rule is ignored whole (both browsers drop it from the style sheet); with the syntax `*` it is the descriptor that is ignored.
 *
 * Of two valid rules for one name the later wins ("the last such one in document order"). Nothing is registered by an `@property` inside a style rule, an `@function` or a `@mixin`: the draft
 * places it at the top of a sheet (and in the group rules the cascade enters) and, with private properties, inside `@private`, which this library does not have.
 *
 * **CSSOM of several names (the CSSWG's resolution of w3c/csswg-drafts#14227, 2026-08-06):** the draft's Issue box still says "The CSSOM for multi-name @property rules has not been resolved on by the
 * CSSWG"; the resolution is "a single object with a `names` array [...] and return empty string for the `name` in that case". A rule of several names is one rule here (css_cssom_rule.h: its `name`
 * is empty, gtext_cssom_property_get_names() has every name, its text lists them all; the typed model's names are every one).
 *
 * ## A caller's registration wins
 *
 * "A custom property registration from CSS.registerProperty() further wins over any @property rules for the same <custom-property-name>." A name the registry
 * has (gtext_css_registry_register_custom()) is not registered by a rule: the rule is reported as ignored (GTEXT_CSS_AT_PROP_CALLER_WINS) and the caller's
 * registration is what styles are computed with.
 *
 * ## Use
 *
 * Make a set with gtext_css_at_property_set_new(), give it each stylesheet's tree in the order of the document with gtext_css_at_property_collect(), and hand
 * the set to the compute context (GTEXT_CSS_Compute_Options::property_rules). The set must outlive the context and is not changed while a context uses it.
 * A conditional rule (`@media`, `@supports` ...) that holds an `@property` is entered only when the `active` callback says so; without one it is
 * GTEXT_CSS_E_UNSUPPORTED naming `active_rule`, never a guess (as for layers, css_layer.h).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_AT_PROPERTY_H
#define GHOTI_IO_GTEXT_CSS_CSS_AT_PROPERTY_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_layer.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Why a rule or a descriptor was not used. */
typedef enum {
  GTEXT_CSS_AT_PROP_BAD_PRELUDE = 0,   ///< The prelude is not `<custom-property-name>#` (a rule) or `<custom-property-name> <css-type>? [: <declaration-value>? ]?` (a statement): the rule is ignored.
  GTEXT_CSS_AT_PROP_BAD_SYNTAX,        ///< The `syntax` descriptor is not a string that is a syntax definition: it is ignored (the rule has the syntax it had without it, `*` for none).
  GTEXT_CSS_AT_PROP_BAD_INHERITS,      ///< The `inherits` descriptor is not `true` or `false`: it is ignored (the rule inherits, as it does when it says nothing).
  GTEXT_CSS_AT_PROP_BAD_INITIAL,       ///< The `initial-value` is not a value of the syntax: it is ignored (the initial value is the guaranteed-invalid value).
  GTEXT_CSS_AT_PROP_NOT_INDEPENDENT,   ///< The `initial-value` needs the element (`3em`, `currentcolor`) or holds a substitution function: the rule is ignored (for the syntax `*`, the descriptor is).
  GTEXT_CSS_AT_PROP_UNKNOWN_DESCRIPTOR, ///< A declaration of a name that is no descriptor of `@property`, or a descriptor with `!important`: it is ignored and the rule stands.
  GTEXT_CSS_AT_PROP_SUPERSEDED,        ///< An earlier valid rule for the name: the later one wins, and this reports the earlier.
  GTEXT_CSS_AT_PROP_CALLER_WINS        ///< The registry has a registration of the name: the rule's registration of that name is ignored.
} GTEXT_CSS_At_Property_Reason;

/** @brief One rule or descriptor that was ignored or replaced. The strings are owned by the set. */
typedef struct {
  GTEXT_CSS_At_Property_Reason reason;
  GTEXT_CSS_String name;   ///< The custom property's name; for an unknown descriptor, its name; for a bad prelude, the prelude as written.
  GTEXT_CSS_Id rule;       ///< The rule's id in its tree.
  GTEXT_CSS_Span span;     ///< The rule's span in its tree (for a descriptor, the declaration's).
  size_t sheet;            ///< Which tree: 0 for the first given to gtext_css_at_property_collect().
  const char * message;    ///< A static sentence saying which rule of the draft it applies; never NULL.
} GTEXT_CSS_At_Property_Diagnostic;

/** @brief The allocator and limits of a set. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the registry's.
  void * user;                        ///< Passed to `active`.
  GTEXT_CSS_Layer_Active_Fn active;   ///< Whether a conditional group rule that holds an `@property` applies. NULL: such a rule is GTEXT_CSS_E_UNSUPPORTED, naming `active_rule`.
  size_t max_properties;              ///< Most registrations the set holds. Default 4,096; at most 2^24. One more is GTEXT_CSS_E_LIMIT_REGISTRY.
  size_t max_diagnostics;             ///< Most diagnostics kept (the rest are counted). Default 1,024; at most 2^20.
  size_t max_rule_depth;              ///< Deepest nesting of rules read. Default 4,096; at most 1,048,576. One more is GTEXT_CSS_E_LIMIT_DEPTH.
  /** As `active`, but a failure is the reader's: a status other than GTEXT_CSS_OK stops the reading and is the call's (gtext_css_conditions_active_rule_status()). Used in place of `active` when given. */
  GTEXT_CSS_Status (*active_status)(void * user, const GTEXT_CSS_Rule * rule, bool * applies);
} GTEXT_CSS_At_Property_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_At_Property_Options gtext_css_at_property_options_default(void);

/** @brief What reading a stylesheet reports besides its status. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;  ///< What the call returned.
  const char * message;     ///< A static description; never NULL after a call.
  const char * detail;      ///< A static sentence saying why, or NULL.
  GTEXT_CSS_Span span;      ///< On a failure: the rule concerned.
  size_t work;              ///< Units of work done (rules visited, descriptors read, probes), for the quadratic suite.
} GTEXT_CSS_At_Property_Result;

/** @brief The registrations that the `@property` rules of some stylesheets make (the struct is private). */
typedef struct GTEXT_CSS_At_Property_Set GTEXT_CSS_At_Property_Set;

/**
 * @brief Make an empty set.
 *
 * @param registry The registry, frozen. Not NULL; it must outlive the set. Its caller registrations win over a rule's, and it compiles the rules' syntaxes.
 * @param options NULL for the defaults.
 * @param out Receives the set; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_at_property_set_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_At_Property_Options * options,
    GTEXT_CSS_At_Property_Set ** out);

/** @brief Free a set and everything it holds; NULL is ignored. */
GTEXT_API void gtext_css_at_property_set_free(GTEXT_CSS_At_Property_Set * set);

/**
 * @brief Read the `@property` rules of one stylesheet, after those of the sheets already given.
 *
 * Reads, without recursion, the rules of the tree, entering `@layer` blocks and (when `active` says so) conditional group rules. Style rules are not
 * entered: an `@property` is not valid inside one.
 *
 * @param set The set. Not NULL.
 * @param tree A tree of a stylesheet (or of block contents). Not NULL.
 * @param result NULL, or filled; on a failure its `span` is the rule concerned.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED (see `active`); GTEXT_CSS_E_LIMIT_REGISTRY; GTEXT_CSS_E_LIMIT_DEPTH; GTEXT_CSS_E_LIMIT_WORK;
 *   GTEXT_CSS_E_OOM. What the rules before a failure registered stays.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_at_property_collect(GTEXT_CSS_At_Property_Set * set, const GTEXT_CSS_Tree * tree, GTEXT_CSS_At_Property_Result * result);

/**
 * @brief Read the `@property` rules among top-level rules that are not in one array, as one stylesheet: the rules of a sheet whose rules each keep their own tree.
 *
 * The same reading as gtext_css_at_property_collect() over the rules given, in order, counted as one sheet (the `sheet` of a diagnostic is the same for all of them).
 *
 * @param set The set. Not NULL.
 * @param rules The top-level rules of the sheet, in order; none is NULL. NULL with a `count` of 0 is an empty sheet.
 * @param count How many.
 * @param result NULL, or filled; on a failure its `span` is the rule concerned.
 * @return As gtext_css_at_property_collect(); GTEXT_CSS_E_INVALID for a NULL rule. What the rules before a failure registered stays.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_at_property_collect_rules(GTEXT_CSS_At_Property_Set * set, const GTEXT_CSS_Rule * const * rules, size_t count,
    GTEXT_CSS_At_Property_Result * result);

/** @brief How many custom properties the rules registered. NULL is 0. */
GTEXT_API size_t gtext_css_at_property_count(const GTEXT_CSS_At_Property_Set * set);

/** @brief The `index`th registration, in the order of the names' first registration; false for an index the set does not have. */
GTEXT_API bool gtext_css_at_property_at(const GTEXT_CSS_At_Property_Set * set, size_t index, GTEXT_CSS_Custom_Info * info);

/** @brief The registration of a name (case-sensitive); false if the rules made none. */
GTEXT_API bool gtext_css_at_property_find(const GTEXT_CSS_At_Property_Set * set, const char * name, size_t length, GTEXT_CSS_Custom_Info * info);

/** @brief How many diagnostics were raised in all (kept or not). NULL is 0. */
GTEXT_API size_t gtext_css_at_property_diagnostic_total(const GTEXT_CSS_At_Property_Set * set);

/** @brief The `index`th kept diagnostic, in the order raised; false past the end. */
GTEXT_API bool gtext_css_at_property_diagnostic(const GTEXT_CSS_At_Property_Set * set, size_t index, GTEXT_CSS_At_Property_Diagnostic * out);

/** @brief The name of a reason ("bad-syntax" ...); static. */
GTEXT_API const char * gtext_css_at_property_reason_name(GTEXT_CSS_At_Property_Reason reason);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_AT_PROPERTY_H
