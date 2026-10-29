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
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_media.h>
#include <ghoti.io/text/css/css_registry.h>
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

/* ---- the conditions object ---------------------------------------------- */

/** @brief The evaluator: see the file's description. */
typedef struct GTEXT_CSS_Conditions GTEXT_CSS_Conditions;

/** @brief What a conditions object is made of. Initialize it with gtext_css_conditions_options_default(). */
typedef struct {
  const GTEXT_Allocator * allocator;            ///< NULL for the default.
  const GTEXT_CSS_Media_Environment * media;    ///< The media environment (copied); NULL: gtext_css_media_environment_default(NULL).
  GTEXT_CSS_Condition_Defs * defs;              ///< The `@custom-media` definitions; NULL for none. It is sealed by the call and retained.
  bool custom_media;                            ///< `(--name)` uses the definitions (the served draft). Default true; false is the browsers' behavior.
  size_t max_work;                              ///< Most units of work of one evaluation. Default 2^24; one more is GTEXT_CSS_E_LIMIT_WORK.
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
  const char * detail;       ///< After GTEXT_CSS_E_UNSUPPORTED: a static sentence naming what is missing (the unit, the callback).
  GTEXT_CSS_Span where;      ///< After a failure: the span of the node it was reached at.
  struct GTEXT_CSS_Condition_Call_Internal * internal; ///< Private.
} GTEXT_CSS_Condition_Call;

/** @brief Zero a call. */
GTEXT_API void gtext_css_condition_call_init(GTEXT_CSS_Condition_Call * call);

/** @brief Release a call's memo and reset it. Safe on NULL and on a zeroed call. */
GTEXT_API void gtext_css_condition_call_free(GTEXT_CSS_Condition_Call * call);

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
