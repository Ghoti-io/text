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
 * @file css_condition.h
 * @brief The conditions object: one evaluator of the conditions of CSS (media queries now; `@supports`, `@container` and `@when` as they land), and the
 *   `@custom-media` definitions.
 *
 * ## The conditions object
 *
 * A @ref GTEXT_CSS_Conditions holds a copy of the media environment, the `@custom-media` definitions (sealed) and the limits. It is **immutable after
 * creation and reference counted with atomics, and holds no cache and no lock**, so any number of threads evaluate with one. All memo lives in the
 * caller's @ref GTEXT_CSS_Condition_Call (or, with none, in the call). A change of environment (a resize, a scheme switch) is a **new**
 * conditions object, which is cheap, and is what a rule index is rebuilt against.
 *
 * ## Custom media (Media Queries 5, 10)
 *
 * `@custom-media --name <media-query-list> | true | false` defines a name; `(--name)` in a boolean position uses it (also `not (--name)`). A custom media
 * is true if its list evaluates to true and false otherwise; "the last one alone" of several definitions of a name counts; a cycle makes **every name
 * on it undefined** (an undefined name is `<general-enclosed>`: unknown). The cycles are found when the conditions object is made, in one pass
 * over the definitions, so a definition that a short circuit would never reach is undefined all the same. Each definition is evaluated at most once for
 * a call context, so a chain of n definitions costs O(n). With the option `custom_media` off (the behavior of every shipping browser) `(--name)` is
 * `<general-enclosed>` whatever is defined.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_CONDITION_H
#define GHOTI_IO_GTEXT_CSS_CSS_CONDITION_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_container.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_media.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- custom media definitions ------------------------------------------ */

/** @brief The `@custom-media` definitions of the style sheets given to it. Mutable until the first conditions object is made from it; not shared while mutable. */
typedef struct GTEXT_CSS_Condition_Defs GTEXT_CSS_Condition_Defs;

/** @brief A new, empty set of definitions. `allocator` NULL for the default. */
GTEXT_API GTEXT_CSS_Status gtext_css_condition_defs_new(const GTEXT_Allocator * allocator, GTEXT_CSS_Condition_Defs ** out);

/** @brief Free the definitions (and let go of the lists they hold). Conditions objects made from them keep their own reference. NULL is ignored. */
GTEXT_API void gtext_css_condition_defs_free(GTEXT_CSS_Condition_Defs * defs);

/** @brief The number of definitions added (a name defined twice counts twice). */
GTEXT_API size_t gtext_css_condition_defs_count(const GTEXT_CSS_Condition_Defs * defs);

/**
 * @brief Add the definition of an `@custom-media` rule from its prelude: `<extension-name> [ <media-query-list> | true | false ]`.
 *
 * A prelude that does not match (no `--` name, nothing after it) is not a definition: the call returns GTEXT_CSS_OK with `*added` false.
 * A later definition of a name replaces an earlier one.
 *
 * @param defs The definitions. Not NULL.
 * @param values The prelude's component values.
 * @param count Their number.
 * @param options For the parse of the list (NULL for the defaults).
 * @param added NULL, or receives whether a definition was added.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (the definitions are sealed); GTEXT_CSS_E_OOM; the limits of the parse.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_condition_defs_add_custom_media(GTEXT_CSS_Condition_Defs * defs, const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Media_Options * options, bool * added);

/** @brief gtext_css_condition_defs_add_custom_media() from text: `--narrow (max-width: 30em)`. */
GTEXT_API GTEXT_CSS_Status gtext_css_condition_defs_add_custom_media_text(GTEXT_CSS_Condition_Defs * defs, const char * text, size_t length,
    const GTEXT_CSS_Media_Options * options, bool * added);

/* ---- @supports ------------------------------------------------------------ */

/** @name Font technologies and formats
 *  Bits of GTEXT_CSS_Support_Policy::font_tech (the idents of `<font-tech>`) and ::font_format (the idents of `<font-format>`).
 *  @{ */
#define GTEXT_CSS_FONT_TECH_FEATURES_OPENTYPE 0x1u
#define GTEXT_CSS_FONT_TECH_FEATURES_AAT 0x2u
#define GTEXT_CSS_FONT_TECH_FEATURES_GRAPHITE 0x4u
#define GTEXT_CSS_FONT_TECH_COLOR_COLRV0 0x8u
#define GTEXT_CSS_FONT_TECH_COLOR_COLRV1 0x10u
#define GTEXT_CSS_FONT_TECH_COLOR_SVG 0x20u
#define GTEXT_CSS_FONT_TECH_COLOR_SBIX 0x40u
#define GTEXT_CSS_FONT_TECH_COLOR_CBDT 0x80u
#define GTEXT_CSS_FONT_TECH_VARIATIONS 0x100u
#define GTEXT_CSS_FONT_TECH_PALETTES 0x200u
#define GTEXT_CSS_FONT_TECH_INCREMENTAL 0x400u
#define GTEXT_CSS_FONT_FORMAT_COLLECTION 0x1u
#define GTEXT_CSS_FONT_FORMAT_EMBEDDED_OPENTYPE 0x2u
#define GTEXT_CSS_FONT_FORMAT_OPENTYPE 0x4u
#define GTEXT_CSS_FONT_FORMAT_SVG 0x8u
#define GTEXT_CSS_FONT_FORMAT_TRUETYPE 0x10u
#define GTEXT_CSS_FONT_FORMAT_WOFF 0x20u
#define GTEXT_CSS_FONT_FORMAT_WOFF2 0x40u
/** @} */

/**
 * @struct GTEXT_CSS_Support_Policy
 * @brief What a consumer supports that the library cannot know: the answers of `@supports`. A zeroed struct is the default: every property of the registry,
 *   no font technology, no font format, no named feature, no environment variable, every at-rule this library implements.
 */
typedef struct {
  void * user;                                                          ///< Passed to the callbacks.
  /** Narrow the properties: a renderer that does not implement `backdrop-filter` says so. NULL: every property of the registry is supported. */
  bool (*property_supported)(void * user, GTEXT_CSS_Property_Id property);
  uint32_t font_tech;                                                   ///< GTEXT_CSS_FONT_TECH_* the consumer can render: `font-tech()`.
  uint32_t font_format;                                                 ///< GTEXT_CSS_FONT_FORMAT_* it can load: `font-format()`.
  /** `named-feature(ident)`. NULL: none is supported. */
  bool (*named_feature)(void * user, const GTEXT_CSS_String * name);
  /** `env(ident)`. NULL: none is supported. */
  bool (*env_variable)(void * user, const GTEXT_CSS_String * name);
  /** Narrow `at-rule(@name)` (`name` without the `@`, lower case). NULL: every at-rule this library implements. */
  bool (*at_rule_supported)(void * user, const GTEXT_CSS_String * name);
} GTEXT_CSS_Support_Policy;

/* ---- the conditions object ---------------------------------------------- */

/** @brief The evaluator: see the file's description. */
typedef struct GTEXT_CSS_Conditions GTEXT_CSS_Conditions;

/** @brief What a conditions object is made of. Initialize it with gtext_css_conditions_options_default(). */
typedef struct {
  const GTEXT_Allocator * allocator;            ///< NULL for the default.
  const GTEXT_CSS_Media_Environment * media;    ///< The media environment (copied); NULL: gtext_css_media_environment_default(NULL).
  GTEXT_CSS_Condition_Defs * defs;              ///< The `@custom-media` definitions; NULL for none. It is sealed by the call and retained.
  bool custom_media;                            ///< `(--name)` uses the definitions (the served draft). Default true; false is the browsers' behavior.
  const GTEXT_CSS_Support_Policy * policy;      ///< What `@supports` is answered from (copied); NULL: the default policy.
  const GTEXT_CSS_Element_Interface * element_interface; ///< The element interface `selector()` is checked against (borrowed: it must outlive the object); NULL: a selector() is checked for syntax only.
  size_t max_work;                              ///< Most units of work of one evaluation. Default 2^24; one more is GTEXT_CSS_E_LIMIT_WORK.
  const GTEXT_CSS_Container_Interface * container; ///< What the caller knows about its containers (copied; css_container.h); NULL: no container is a size or scroll-state container, and a style query asks nothing of one.
  size_t max_container_requests;                ///< Most distinct (name, kinds) requests a call context resolves, and an index accepts. Default 4,096; one more is GTEXT_CSS_E_LIMIT_CONTAINER.
} GTEXT_CSS_Conditions_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Conditions_Options gtext_css_conditions_options_default(void);

/**
 * @brief Make a conditions object.
 *
 * @param registry The property registry (for `@supports`); may be NULL until the supports half needs it. Not retained: it must outlive the object.
 * @param options NULL for the defaults.
 * @param out Receives the object (one owner). Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (an environment that does not pass gtext_css_media_environment_check()); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Conditions_Options * options, GTEXT_CSS_Conditions ** out);

/** @brief Another owner. Returns `conditions`; NULL is ignored. */
GTEXT_API GTEXT_CSS_Conditions * gtext_css_conditions_retain(const GTEXT_CSS_Conditions * conditions);

/** @brief One owner fewer; the last frees the object. NULL is ignored. */
GTEXT_API void gtext_css_conditions_free(const GTEXT_CSS_Conditions * conditions);

/** @brief The media environment the object evaluates against (a copy; valid for the object's life). */
GTEXT_API const GTEXT_CSS_Media_Environment * gtext_css_conditions_media_environment(const GTEXT_CSS_Conditions * conditions);

/** @brief The number of custom media names that a cycle makes undefined. */
GTEXT_API size_t gtext_css_conditions_cycle_count(const GTEXT_CSS_Conditions * conditions);

/** @brief The `index`th name on a cycle (as written, `--a`), in the order of the definitions; NULL past the end. Valid for the object's life. */
GTEXT_API const char * gtext_css_conditions_cycle_name(const GTEXT_CSS_Conditions * conditions, size_t index);

/**
 * @struct GTEXT_CSS_Condition_Call
 * @brief The caller's context of a run of evaluations: the memo of the custom media, and the counters. Zero it (or use gtext_css_condition_call_init()) before
 *   the first use, release it with gtext_css_condition_call_free(). One call is used by one thread at a time; any number of calls may use one conditions object.
 *
 * Keeping one across the evaluations of a sheet (as the rule index does) evaluates each custom media once for the whole run.
 */
typedef struct {
  size_t work;               ///< Units of work done by the last evaluation (the limit counts these).
  size_t node_visits;        ///< Condition nodes visited, over all evaluations of the call.
  size_t defs_evaluated;     ///< Custom media definitions evaluated, over all evaluations of the call.
  size_t memo_hits;          ///< Uses of a custom media answered from the memo.
  size_t container_steps;    ///< Ancestors visited looking for a query container, over all resolutions of the call.
  size_t container_hits;     ///< Resolutions answered from the container cache.
  size_t container_requests; ///< Distinct (name, kinds) requests the call has resolved.
  size_t container_evaluations; ///< Container conditions evaluated against a container (not answered from the memo).
  const char * detail;       ///< After GTEXT_CSS_E_UNSUPPORTED: a static sentence naming what is missing (the unit, the callback).
  GTEXT_CSS_Span where;      ///< After a failure: the span of the node it was reached at.
  struct GTEXT_CSS_Condition_Call_Internal * internal; ///< Private.
} GTEXT_CSS_Condition_Call;

/** @brief Zero a call. */
GTEXT_API void gtext_css_condition_call_init(GTEXT_CSS_Condition_Call * call);

/** @brief Release a call's memo and reset it. Safe on NULL and on a zeroed call. */
GTEXT_API void gtext_css_condition_call_free(GTEXT_CSS_Condition_Call * call);

/** @brief Forget the container cache and the memo of the call, keeping its buffers and the counters: do this when the tree, a computed style or a size has changed. Safe on NULL. */
GTEXT_API void gtext_css_condition_call_reset(GTEXT_CSS_Condition_Call * call);

/**
 * @brief Whether a media query list is true: some query of it is true; unknown is false (Media Queries 4, 3.1). The empty list is true.
 *
 * @param conditions The evaluator. Not NULL.
 * @param list The list. Not NULL.
 * @param call NULL, or the caller's context.
 * @param matches Receives the answer; written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_UNSUPPORTED (a unit that needs a font metric the environment lacks; `call->detail` names it);
 *   GTEXT_CSS_E_LIMIT_WORK; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_matches_list(const GTEXT_CSS_Conditions * conditions, const GTEXT_CSS_Media_List * list,
    GTEXT_CSS_Condition_Call * call, bool * matches);

/** @brief The three-valued answer for query `index` of the list (before unknown becomes false): see gtext_css_conditions_matches_list(). */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_evaluate_query(const GTEXT_CSS_Conditions * conditions, const GTEXT_CSS_Media_List * list, size_t index,
    GTEXT_CSS_Condition_Call * call, GTEXT_CSS_Tristate * result);

/**
 * @brief `matchMedia(text).matches`: parse a media query list from text and evaluate it.
 *
 * @param text UTF-8. NULL only with a `length` of 0.
 * @param length Its length.
 * @param call NULL, or the caller's context.
 * @param matches Receives the answer. Not NULL.
 * @return As gtext_css_conditions_matches_list(), and the limits of the parse.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_matches_media(const GTEXT_CSS_Conditions * conditions, const char * text, size_t length,
    GTEXT_CSS_Condition_Call * call, bool * matches);

/** @brief A parsed `<supports-condition>`: the same immutable AST and evaluator as a media query list (one condition). Free it with gtext_css_media_list_free(). */
typedef GTEXT_CSS_Media_List GTEXT_CSS_Supports_Condition;

/**
 * @brief Parse a `<supports-condition>`: `not`, `and`, `or`, parentheses, `(<declaration>)`, `selector()`, `font-tech()`, `font-format()`, `at-rule()`, `named-feature()`,
 *   `env()` and `<general-enclosed>` (unknown, false).
 *
 * @param bare_declaration Also accept a declaration without parentheses (`display: grid`), which is what `CSS.supports(text)` and the `supports()` of `@import` allow.
 * @return As gtext_css_media_list_parse_values(): an invalid condition is OK, replaced (the rule never applies), with a diagnostic.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_supports_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, bool bare_declaration, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Supports_Condition ** out, GTEXT_CSS_Media_Result * result);

/** @brief gtext_css_supports_parse_values() from text. */
GTEXT_API GTEXT_CSS_Status gtext_css_supports_parse(const char * text, size_t length, bool bare_declaration, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Supports_Condition ** out, GTEXT_CSS_Media_Result * result);

/** @brief The three-valued answer of a parsed supports condition. @return As gtext_css_conditions_matches_list(); GTEXT_CSS_E_UNSUPPORTED naming `registry` when a declaration is asked and there is none. */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_evaluate_supports(const GTEXT_CSS_Conditions * conditions, const GTEXT_CSS_Supports_Condition * condition,
    GTEXT_CSS_Condition_Call * call, GTEXT_CSS_Tristate * result);

/**
 * @brief A parsed `<boolean-condition>` of `@when` and `@else` (Conditional 5, 3 and 4): a boolean algebra with `media( <media-feature> )` and `supports( ... )` as leaves
 *   (and the supports functions `font-tech()`, `selector()` ... directly, as the draft's own examples write them). The same immutable AST as a media query list (one condition).
 *   Free it with gtext_css_media_list_free().
 */
typedef GTEXT_CSS_Media_List GTEXT_CSS_Boolean_Condition;

/**
 * @brief Parse a `<boolean-condition>`. An invalid condition is OK, replaced (the rule never applies), with a diagnostic; `<general-enclosed>` is unknown. A `media()` whose
 *   feature is known and written wrongly is false; one whose feature is unknown is unknown.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_when_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Boolean_Condition ** out, GTEXT_CSS_Media_Result * result);

/** @brief gtext_css_when_parse_values() from text. */
GTEXT_API GTEXT_CSS_Status gtext_css_when_parse(const char * text, size_t length, const GTEXT_CSS_Media_Options * options, GTEXT_CSS_Boolean_Condition ** out,
    GTEXT_CSS_Media_Result * result);

/**
 * @brief The three-valued answer of a parsed boolean condition. @return As gtext_css_conditions_matches_list().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_evaluate_boolean(const GTEXT_CSS_Conditions * conditions, const GTEXT_CSS_Boolean_Condition * condition,
    GTEXT_CSS_Condition_Call * call, GTEXT_CSS_Tristate * result);

/**
 * @brief Whether a container query list is true for an element: some condition of it is true for the container it selects. Unknown, and no container, are false.
 *
 * @param conditions The evaluator, made with an element interface (its `inheritance_parent`) and a container interface (css_container.h). Not NULL.
 * @param query The list. Not NULL.
 * @param element The element the rule's style applies to. Not NULL.
 * @param from_element Begin with the element itself, not its inheritance parent: the style is a pseudo-element's (`::before`), whose originating element is a candidate container.
 * @param call NULL, or the caller's context; the cache that makes a walk over a whole tree linear.
 * @param matches Receives the answer; written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_UNSUPPORTED (a callback the query needs is absent: `call->detail` names it, `container.size`); GTEXT_CSS_E_LIMIT_WORK; GTEXT_CSS_E_LIMIT_CONTAINER; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_evaluate_container(const GTEXT_CSS_Conditions * conditions, const GTEXT_CSS_Container_Query * query,
    GTEXT_CSS_Element_Handle element, bool from_element, GTEXT_CSS_Condition_Call * call, bool * matches);

/**
 * @brief The size of the query container of an element on an axis, for a `cqw`, `cqh`, `cqi` or `cqb` unit: the shape of GTEXT_CSS_Environment::container_size, with the
 *   conditions and the call context as `user`. A caller's `container_size` callback calls it, so that a `cqw` in a declaration and a `width` in a `@container` query are one fact.
 *
 * "The query container for each axis is the nearest ancestor container that accepts container size queries on that axis." Returns false if there is none (the unit then falls back
 * to the small viewport), or the container has no principal box.
 */
GTEXT_API bool gtext_css_container_environment_size(const GTEXT_CSS_Conditions * conditions, GTEXT_CSS_Condition_Call * call, GTEXT_CSS_Element_Handle element,
    GTEXT_CSS_Container_Axis axis, double * px);

/**
 * @brief A filter for the tree walkers that stay filters (gtext_css_cascade_collect_layers(), gtext_css_at_property_collect()): `@media`, `@supports`, `@when` and `@else` are
 *   evaluated; `@container` and `@starting-style` are true, because the condition depends on the element (Cascade 5, 6.4.3). Pass the conditions object as the filter's `user`. An
 *   evaluation that fails is false.
 */
GTEXT_API bool gtext_css_conditions_active_rule(void * conditions, const GTEXT_CSS_Rule * rule);

/**
 * @brief `CSS.supports(property, value)`: whether the registry knows the property and the value is valid for it (by the parser the cascade uses, quirks off), and the policy
 *   does not narrow the property out. A custom property is supported with any value.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_supports(const GTEXT_CSS_Conditions * conditions, const char * property, const char * value, bool * result);

/** @brief `CSS.supports(conditionText)`: a bare declaration or a supports condition. Unknown is false. */
GTEXT_API GTEXT_CSS_Status gtext_css_conditions_supports_text(const GTEXT_CSS_Conditions * conditions, const char * text, size_t length, bool * result);

/** @brief What a condition depends on, as data (the invalidation summary; C6 consumes it). */
typedef struct {
  uint32_t environment;  ///< Bit `1 << k` for each environment aspect: 0 viewport, 1 device, 2 resolution, 3 color, 4 display, 5 input, 6 scheme, 7 preference, 8 environment, 9 extension.
  uint64_t features[2];  ///< Bit i of the pair: media feature row i is mentioned (rows 0 to 127).
  bool custom_media;     ///< Mentions a `(--name)`.
  bool unknown_features; ///< Mentions a feature the table does not list.
} GTEXT_CSS_Condition_Dependencies;

/** @brief The dependencies of a media query list. */
GTEXT_API void gtext_css_media_list_dependencies(const GTEXT_CSS_Media_List * list, GTEXT_CSS_Condition_Dependencies * out);

/**
 * @brief Answer the test of an `if()` condition (the shape of GTEXT_CSS_Environment::condition, for a caller whose callback has a `user` of its own):
 *   parse `text` as a media query (or, with the supports half, a supports condition) and evaluate it. Unknown, an error and a text that is not a condition
 *   are GTEXT_CSS_TRI_UNKNOWN or false as the draft says (Values 5, 8.3: unknown at the top is false).
 */
GTEXT_API GTEXT_CSS_Tristate gtext_css_conditions_condition(const GTEXT_CSS_Conditions * conditions, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text);

/**
 * @brief Make an environment of the computed-value layer answer `if(media(...))` through this evaluator: fills `condition` and `user`.
 *
 * The text of the callback is parsed as a media query list on each call and the parse is not cached; the text is bounded by its length. `user` is the
 * conditions object, which must outlive the environment's use. Unknown is false. `supports(...)` is answered by the supports half where it is present and
 * is GTEXT_CSS_TRI_UNKNOWN until then.
 */
GTEXT_API void gtext_css_conditions_environment_hook(const GTEXT_CSS_Conditions * conditions, GTEXT_CSS_Environment * env);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_CONDITION_H
