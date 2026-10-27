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
 * Reference: CSS Properties and Values API Level 1 (6 August 2026), 2 ("Registered Custom Properties"), 3 ("The `@property` Rule"), 4.1 and 5 ("Syntax
 * Strings"), as served 2026-10-27; CSS Cascade Level 5 for what a conditional rule does to the rules inside it.
 *
 * ## What a rule has to say
 *
 * An `@property` rule registers a custom property for each name of its prelude (`@property --a, --b { ... }`). It is **valid** when its `syntax` and `inherits`
 * descriptors are both there and valid, and, for a syntax other than `*`, when its `initial-value` is there, is a value of the syntax and is
 * computationally independent (`5px`, `1in`, `10vw`: not `3em`, `currentcolor`, `var(--x)`). A rule that is not valid is ignored whole and reported
 * (GTEXT_CSS_At_Property_Diagnostic); so is the rule of a name that is not a dashed identifier (`@property foo`, `@property --`). A descriptor the rule
 * does not know is ignored and reported, and the rule stands. Of two valid rules for one name, the later wins ("the last such one in document order").
 *
 * **Decided:** the served draft says "While the <custom-property-name> is required, all of the descriptors are optional. When omitted, it matches the behavior
 * of an unregistered custom property." This library follows the plan's rule (a rule without `syntax` or `inherits` is ignored), which is what the pinned
 * web-platform-tests helper still assumes ("The syntax descriptor is required ... The inherits descriptor is required") and what the browsers implement. It is one
 * condition in one function (`rule_valid()` of css_at_property.c), and the reading of the draft is in the tests that quote it.
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
  GTEXT_CSS_AT_PROP_BAD_NAME = 0,      ///< The prelude is not one custom property name (a dashed identifier other than `--`): the rule is ignored. A list of names is not a prelude of this rule.
  GTEXT_CSS_AT_PROP_NO_SYNTAX,         ///< No valid `syntax` descriptor: the rule is ignored.
  GTEXT_CSS_AT_PROP_BAD_SYNTAX,        ///< The `syntax` descriptor is not a string that is a syntax definition (and so there is none): the rule is ignored.
  GTEXT_CSS_AT_PROP_NO_INHERITS,       ///< No valid `inherits` descriptor (it is `true` or `false`): the rule is ignored.
  GTEXT_CSS_AT_PROP_NO_INITIAL,        ///< A syntax other than `*` and no valid `initial-value`: the rule is ignored.
  GTEXT_CSS_AT_PROP_BAD_INITIAL,       ///< The `initial-value` is not a value of the syntax: the rule is ignored (it has none).
  GTEXT_CSS_AT_PROP_NOT_INDEPENDENT,   ///< The `initial-value` needs the element (`3em`, `currentcolor`) or holds a substitution function: the rule is ignored.
  GTEXT_CSS_AT_PROP_UNKNOWN_DESCRIPTOR, ///< A declaration of a name that is no descriptor of `@property`: it is ignored and the rule stands.
  GTEXT_CSS_AT_PROP_SUPERSEDED,        ///< An earlier valid rule for the name: the later one wins, and this reports the earlier.
  GTEXT_CSS_AT_PROP_CALLER_WINS        ///< The registry has a registration of the name: the rule's registration is ignored.
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

/** @brief The name of a reason ("no-syntax" ...); static. */
GTEXT_API const char * gtext_css_at_property_reason_name(GTEXT_CSS_At_Property_Reason reason);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_AT_PROPERTY_H
