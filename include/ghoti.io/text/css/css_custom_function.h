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
 * @file css_custom_function.h
 * @brief `@function` rules: reading them from stylesheets into the custom functions a style is computed with.
 *
 * Reference: CSS Functions and Mixins Module Level 1 (the Editor's Draft pinned in tools/css/DRAFTS_COMMIT), section 2 ("Defining Custom Functions": the `@function` rule,
 * its prelude, its body, the `result` descriptor); CSS Cascade Level 5 for what a layer does to rules of one name; CSS Properties and Values API 1, 5, for the syntax strings
 * of the types.
 *
 * ## What a rule has to say
 *
 * An `@function` rule is `@function --name(<parameters>) [returns <type>] { <body> }`. Each parameter is `--name [<type>] [: <default>]`, a type is one syntax component
 * (`<length>`, `<length>+`, `auto`) or `type(<syntax>)`, and a default has to be a value of the type when both are given ("If a default value and a parameter type are both
 * provided, then the default value must parse successfully according to that parameter type's syntax. Otherwise, the @function rule is invalid.", a default that holds a
 * substitution function is kept: it is parsed when the call is evaluated). A rule is **invalid**, and ignored whole and reported (GTEXT_CSS_Function_Diagnostic), when its name
 * does not begin with two dashes (or is not a function token: `--foo (--x)` is a name and a block), when a parameter is not that, when a name is a parameter twice, or when
 * a type or a default is wrong. A descriptor of the body that is not `result` or a custom property is ignored and reported, and the rule stands.
 *
 * "If more than one @function exists for a given name, then the rule in the stronger cascade layer wins, and rules defined later win within the same layer." The loser is
 * reported (GTEXT_CSS_FN_SUPERSEDED) and is not an error.
 *
 * ## The body
 *
 * The set keeps what the prelude says (the name, the parameters with their types and defaults, the return type) and a pointer to the rule, whose block is the body: the
 * declarations and the conditional group rules among them are read when a call is evaluated, since a condition is a fact of the call. **The trees given to the set must outlive
 * it.**
 *
 * ## Use
 *
 * Make a set with gtext_css_function_set_new(), give it each stylesheet's tree in the order of the document with gtext_css_function_collect(), and hand the set to the compute
 * context. The set must outlive the context and is not changed while a context uses it. A conditional rule (`@media`, `@supports` ...) that holds an `@function` is entered
 * only when the `active` callback says so; without one it is GTEXT_CSS_E_UNSUPPORTED naming `active_rule`, never a guess (as for `@property`, css_at_property.h).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CUSTOM_FUNCTION_H
#define GHOTI_IO_GTEXT_CSS_CSS_CUSTOM_FUNCTION_H

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
  GTEXT_CSS_FN_BAD_PRELUDE = 0,    ///< The prelude is not `--name(<parameters>) [returns <type>]`: the rule is ignored.
  GTEXT_CSS_FN_BAD_NAME,           ///< The function token does not begin with two dashes: the rule is ignored.
  GTEXT_CSS_FN_BAD_PARAMETER,      ///< A parameter is not `--name [<type>] [: <default>]`: the rule is ignored.
  GTEXT_CSS_FN_DUPLICATE_PARAMETER, ///< A parameter name is there twice: the rule is ignored.
  GTEXT_CSS_FN_BAD_TYPE,           ///< A parameter type or the return type is not a type: the rule is ignored.
  GTEXT_CSS_FN_BAD_DEFAULT,        ///< A default is not a value of the parameter's type, or is not a declaration value: the rule is ignored.
  GTEXT_CSS_FN_BAD_RETURNS,        ///< Something follows the parameters that is not `returns <type>`: the rule is ignored.
  GTEXT_CSS_FN_UNKNOWN_DESCRIPTOR, ///< A declaration of the body that is no `result` and no custom property: it is ignored and the rule stands.
  GTEXT_CSS_FN_SUPERSEDED          ///< A stronger or later rule has the name: this one is not the function.
} GTEXT_CSS_Function_Reason;

/** @brief One rule or descriptor that was ignored or replaced. The strings are owned by the set. */
typedef struct {
  GTEXT_CSS_Function_Reason reason;
  GTEXT_CSS_String name;   ///< The function's name; for an unknown descriptor, its name; for a rule with no name, empty.
  GTEXT_CSS_Id rule;       ///< The rule's id in its tree.
  GTEXT_CSS_Span span;     ///< The rule's span in its tree (for a descriptor, the declaration's).
  size_t sheet;            ///< Which tree: 0 for the first given to gtext_css_function_collect().
  const char * message;    ///< A static sentence saying which sentence of the draft it applies; never NULL.
} GTEXT_CSS_Function_Diagnostic;

/** @brief The allocator and limits of a set. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the registry's.
  void * user;                        ///< Passed to `active`.
  GTEXT_CSS_Layer_Active_Fn active;   ///< Whether a conditional group rule that holds an `@function` applies. NULL: such a rule is GTEXT_CSS_E_UNSUPPORTED, naming `active_rule`.
  /** As `active`, but a failure is the reader's: a status other than GTEXT_CSS_OK stops the reading and is the call's. Used in place of `active` when given. */
  GTEXT_CSS_Status (*active_status)(void * user, const GTEXT_CSS_Rule * rule, bool * applies);
  size_t max_functions;               ///< Most functions the set holds. Default 4,096; at most 2^24. One more is GTEXT_CSS_E_LIMIT_REGISTRY.
  size_t max_parameters;              ///< Most parameters of one function. Default 1,024; at most 2^20. One more makes the rule invalid (GTEXT_CSS_FN_BAD_PARAMETER).
  size_t max_diagnostics;             ///< Most diagnostics kept (the rest are counted). Default 1,024; at most 2^20.
  size_t max_rule_depth;              ///< Deepest nesting of rules read. Default 4,096; at most 1,048,576. One more is GTEXT_CSS_E_LIMIT_DEPTH.
} GTEXT_CSS_Function_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Function_Options gtext_css_function_options_default(void);

/** @brief What reading a stylesheet reports besides its status. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;  ///< What the call returned.
  const char * message;     ///< A static description; never NULL after a call.
  const char * detail;      ///< A static sentence saying why, or NULL.
  GTEXT_CSS_Span span;      ///< On a failure: the rule concerned.
  size_t work;              ///< Units of work done (rules visited, descriptors read, parameters read), for the quadratic suite.
} GTEXT_CSS_Function_Result;

/** @brief The functions that the `@function` rules of some stylesheets define (the struct is private). */
typedef struct GTEXT_CSS_Function_Set GTEXT_CSS_Function_Set;

/** @brief One parameter of a function. The strings are owned by the set. */
typedef struct {
  GTEXT_CSS_String name;          ///< `--name`.
  bool has_type;                  ///< A type was written.
  GTEXT_CSS_String type;          ///< The type as a syntax string (`<length>+`, `auto`, or the inside of `type()`), serialized; empty when there is none.
  bool has_default;               ///< A default was written.
  GTEXT_CSS_String default_value; ///< Its text, white space trimmed; empty when there is none.
} GTEXT_CSS_Function_Parameter;

/** @brief A function the rules define. The strings are owned by the set. */
typedef struct {
  GTEXT_CSS_String name;          ///< `--name`, as written.
  size_t parameter_count;
  bool has_returns;               ///< A return type was written (else it is `type(*)`).
  GTEXT_CSS_String returns;       ///< The return type as a syntax string; empty when there is none.
  bool has_result;                ///< The body has a `result` descriptor (else the function always returns the guaranteed-invalid value).
  const GTEXT_CSS_Rule * rule;    ///< The rule; its block is the body. Borrowed from the tree.
  GTEXT_CSS_Layer_Id layer;       ///< The cascade layer the rule is in.
  size_t sheet;                   ///< Which tree the rule is in.
} GTEXT_CSS_Function_Info;

/**
 * @brief Make an empty set.
 *
 * @param registry The registry, frozen. Not NULL; it must outlive the set. It compiles the types.
 * @param options NULL for the defaults.
 * @param out Receives the set; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_function_set_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Function_Options * options, GTEXT_CSS_Function_Set ** out);

/** @brief Free a set and everything it holds; NULL is ignored. */
GTEXT_API void gtext_css_function_set_free(GTEXT_CSS_Function_Set * set);

/**
 * @brief Read the `@function` rules of one stylesheet, after those of the sheets already given.
 *
 * Reads, without recursion, the rules of the tree, entering `@layer` blocks and (when `active` says so) conditional group rules. Style rules are not entered.
 *
 * @param set The set. Not NULL.
 * @param tree A tree of a stylesheet (or of block contents). Not NULL. It must outlive the set.
 * @param result NULL, or filled; on a failure its `span` is the rule concerned.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED (see `active`); GTEXT_CSS_E_LIMIT_REGISTRY; GTEXT_CSS_E_LIMIT_DEPTH; GTEXT_CSS_E_LIMIT_WORK;
 *   GTEXT_CSS_E_OOM. What the rules before a failure defined stays.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_function_collect(GTEXT_CSS_Function_Set * set, const GTEXT_CSS_Tree * tree, GTEXT_CSS_Function_Result * result);

/**
 * @brief Read the `@function` rules among top-level rules that are not in one array, as one stylesheet (as gtext_css_at_property_collect_rules()).
 *
 * @return As gtext_css_function_collect(); GTEXT_CSS_E_INVALID for a NULL rule. What the rules before a failure defined stays.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_function_collect_rules(GTEXT_CSS_Function_Set * set, const GTEXT_CSS_Rule * const * rules, size_t count, GTEXT_CSS_Function_Result * result);

/** @brief How many functions the rules defined. NULL is 0. */
GTEXT_API size_t gtext_css_function_count(const GTEXT_CSS_Function_Set * set);

/** @brief The `index`th function, in the order of the names (bytes); false for an index the set does not have. */
GTEXT_API bool gtext_css_function_at(const GTEXT_CSS_Function_Set * set, size_t index, GTEXT_CSS_Function_Info * info);

/** @brief The function of a name (case-sensitive); false if the rules defined none. */
GTEXT_API bool gtext_css_function_find(const GTEXT_CSS_Function_Set * set, const char * name, size_t length, GTEXT_CSS_Function_Info * info);

/** @brief The `index`th parameter of a function of the set; false for an index the function does not have. */
GTEXT_API bool gtext_css_function_parameter(const GTEXT_CSS_Function_Set * set, const char * name, size_t length, size_t index, GTEXT_CSS_Function_Parameter * out);

/** @brief How many diagnostics were raised in all (kept or not). NULL is 0. */
GTEXT_API size_t gtext_css_function_diagnostic_total(const GTEXT_CSS_Function_Set * set);

/** @brief The `index`th kept diagnostic, in the order raised; false past the end. */
GTEXT_API bool gtext_css_function_diagnostic(const GTEXT_CSS_Function_Set * set, size_t index, GTEXT_CSS_Function_Diagnostic * out);

/** @brief The name of a reason ("bad-name" ...); static. */
GTEXT_API const char * gtext_css_function_reason_name(GTEXT_CSS_Function_Reason reason);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CUSTOM_FUNCTION_H
