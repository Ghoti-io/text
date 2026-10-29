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
 * @file css_layer.h
 * @brief Cascade layers: the registry that gives every `@layer` of an origin its place in the layer order.
 *
 * Reference: CSS Cascading and Inheritance Level 5, section 6.4 "Cascade Layers" (Editor's Draft of 13 April 2026, served 2026-10-26):
 * "Cascade layers are sorted by order of appearance, see 6.4.3 Layer Ordering", "Cascade layers (like declarations) are sorted by the order in
 * which they first are declared, with nested layers grouped within their parent layer. Unlayered rules are sorted later than any layered rules
 * within the same parent layer (if any)", "Each occurrence of an anonymous layer declaration represents a unique cascade layer", and "Cascade
 * layers are scoped to their origin and context".
 *
 * ## The order
 *
 * A layer is named by a path of segments (`framework.theme`), each a name or an anonymous segment that exists once per occurrence. The layers of
 * one origin form a tree under an implicit root (the unlayered styles), and the order is the **post-order** of that tree with children in order
 * of first declaration: a layer's sub-layers come before the layer's own rules, and the root, the unlayered styles, is last. For the document of
 * the specification (`reset.type`, `framework`, `framework.theme`, `reset`, an unlayered rule) that is
 *
 *     reset.type, reset, framework.theme, framework, (unlayered)
 *
 * which is exactly the specification's list: "reset.type, reset (implicit sub-layer), framework.theme, framework (implicit sub-layer),
 * (implicit outer layer)". A layer's **rank** is its place in that list; comparing two layers is comparing two numbers, assigned once by
 * gtext_css_layer_freeze().
 *
 * ## One registry, one context
 *
 * A registry holds three trees, one for each of the user-agent, user and author origins (the animation and transition origins have no
 * layers). Layers are scoped to their origin **and context**: make one registry for each shadow tree context and give it to the candidate set
 * (gtext_css_candidates_use_layers()), so that contexts which order the same names differently are each in their own order. A declaration is never compared
 * with one of another context by layer (the context is compared first), so ranks of different registries are never compared.
 *
 * ## Lifecycle
 *
 * A registry is built by declaring layers, in the order they appear: gtext_css_layer_declare() is idempotent (the same path is the same layer)
 * and gtext_css_cascade_collect_layers() reads them from a parsed sheet. gtext_css_layer_freeze() then assigns the ranks and makes the registry
 * immutable: afterwards a path that exists is still found by gtext_css_layer_declare(), and a new one is GTEXT_CSS_E_INVALID. A frozen
 * registry may be read by any number of threads. Nothing here recurses.
 *
 * ## Limits
 *
 * The number of layers (GTEXT_CSS_E_LIMIT_LAYERS) and the depth of nesting (GTEXT_CSS_E_LIMIT_LAYER_DEPTH), each with the name and place in
 * the result.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_LAYER_H
#define GHOTI_IO_GTEXT_CSS_CSS_LAYER_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Where a declaration comes from (Cascade 5, section 6.2), in ascending order of the cascade's weight of its normal declarations. */
typedef enum {
  GTEXT_CSS_ORIGIN_USER_AGENT = 0, ///< The user agent's default style sheet.
  GTEXT_CSS_ORIGIN_USER,           ///< The user's style sheets.
  GTEXT_CSS_ORIGIN_AUTHOR,         ///< The author's style sheets, style attributes and presentational hints.
  GTEXT_CSS_ORIGIN_ANIMATION,      ///< The keyframe declarations of a running animation (the caller interpolates them).
  GTEXT_CSS_ORIGIN_TRANSITION,     ///< The declarations of a running transition.
  GTEXT_CSS_ORIGIN_COUNT           ///< The number of origins; not one.
} GTEXT_CSS_Origin;

/** @brief The name of an origin ("user-agent", "user", "author", "animation", "transition"); NULL for a value that is not one. Static. */
GTEXT_API const char * gtext_css_origin_name(GTEXT_CSS_Origin origin);

/** @brief A cascade layer of a registry. 0 is none: the declaration is **unlayered**, in the implicit final layer of its origin. */
typedef uint32_t GTEXT_CSS_Layer_Id;

/** @brief The id of no layer: unlayered. */
#define GTEXT_CSS_LAYER_NONE ((GTEXT_CSS_Layer_Id)0)

/** @brief A registry of layers (the struct is private). */
typedef struct GTEXT_CSS_Layer_Registry GTEXT_CSS_Layer_Registry;

/** @brief The allocator and limits of a layer registry. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator; ///< NULL for the default.
  size_t max_layers;                 ///< Most layers, anonymous ones included. Default 65,536; at most 2^24. One more is GTEXT_CSS_E_LIMIT_LAYERS.
  size_t max_depth;                  ///< Deepest nesting (`a.b.c` is 3). Default 256; at most 65,536. One more is GTEXT_CSS_E_LIMIT_LAYER_DEPTH.
  uint64_t hash_seed;                ///< Keys the lookup of a name, which no choice of names an author makes may defeat. 0 (the default): a seed drawn from the addresses of the registry.
} GTEXT_CSS_Layer_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Layer_Options gtext_css_layer_options_default(void);

/** @brief What a layer call reports. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;   ///< What the call returned.
  const char * message;      ///< A static description; never NULL after a call.
  char name[96];             ///< The layer concerned (its path, cut to fit), or what a limit was reached at.
  const char * detail;       ///< A static sentence saying which rule was broken, or NULL.
  GTEXT_CSS_Span span;       ///< collect_layers(): the rule concerned (zeroed otherwise).
  size_t work;               ///< Units of work done (nodes visited, probes), for the quadratic suite.
} GTEXT_CSS_Layer_Result;

/**
 * @brief Make an empty registry.
 *
 * @param options NULL for the defaults.
 * @param out Receives it; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID (a NULL `out`) or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_layer_registry_new(const GTEXT_CSS_Layer_Options * options, GTEXT_CSS_Layer_Registry ** out);

/** @brief Free a registry; NULL is ignored. */
GTEXT_API void gtext_css_layer_registry_free(GTEXT_CSS_Layer_Registry * registry);

/**
 * @brief Declare the layer of a path under a parent, and each layer above it that does not exist: `a.b.c` declares `a`, `a.b` and `a.b.c`.
 *
 * Idempotent: a path that exists is its layer. A path is segments separated by `.`; each segment is a non-empty name and not a CSS-wide
 * keyword (`initial`, `inherit`, `unset`, `revert`, `revert-layer`, `revert-rule`: "reserved for future use"), compared by bytes. A `.` of a name is written `\\.` and a
 * backslash `\\\\` (no other escape is read), so that the path gtext_css_layer_path() writes is the path that declares and finds the layer: `a\\.b` is the one
 * layer named `a.b`, `a.b` is the layer `b` in `a`. A name is also declared with gtext_css_layer_declare_segment(), which reads nothing.
 *
 * @param registry The registry. Not NULL.
 * @param origin The user-agent, user or author origin.
 * @param parent The layer the path is nested in, or GTEXT_CSS_LAYER_NONE for the top level.
 * @param path The path; need not be terminated.
 * @param length Its length.
 * @param out Receives the layer; NULL if not wanted. Written only on success.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, an origin without layers, a parent of another origin, an empty segment or a CSS-wide keyword, or a new
 *   layer in a frozen registry); GTEXT_CSS_E_LIMIT_LAYERS; GTEXT_CSS_E_LIMIT_LAYER_DEPTH; GTEXT_CSS_E_OOM. The layers made before a failure remain.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_layer_declare(GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent,
    const char * path, size_t length, GTEXT_CSS_Layer_Id * out, GTEXT_CSS_Layer_Result * result);

/** @brief As gtext_css_layer_declare() for one segment, taken as it is (a `.` in it is part of the name; the CSS-wide keywords are still refused). */
GTEXT_API GTEXT_CSS_Status gtext_css_layer_declare_segment(GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent,
    const char * name, size_t length, GTEXT_CSS_Layer_Id * out, GTEXT_CSS_Layer_Result * result);

/**
 * @brief Declare an anonymous layer under a parent: a new one each time ("Each occurrence of an anonymous layer declaration represents a unique
 *   cascade layer"). It cannot be named from outside; its own sub-layers are found by their names under it.
 *
 * @return As gtext_css_layer_declare().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_layer_declare_anonymous(GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent,
    GTEXT_CSS_Layer_Id * out, GTEXT_CSS_Layer_Result * result);

/**
 * @brief Find a path without declaring it.
 *
 * @return The layer; GTEXT_CSS_LAYER_NONE for a path that is not declared, an empty one or a bad argument.
 */
GTEXT_API GTEXT_CSS_Layer_Id gtext_css_layer_find(const GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent,
    const char * path, size_t length);

/**
 * @brief Assign the ranks and make the registry immutable. Idempotent.
 *
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_layer_freeze(GTEXT_CSS_Layer_Registry * registry);

/** @brief Whether the registry is frozen. */
GTEXT_API bool gtext_css_layer_is_frozen(const GTEXT_CSS_Layer_Registry * registry);

/** @brief How many layers the registry holds in an origin (the unlayered styles not counted). NULL is 0. */
GTEXT_API size_t gtext_css_layer_count(const GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin);

/** @brief What the registry knows of one layer. */
typedef struct {
  GTEXT_CSS_Layer_Id id;          ///< The layer.
  GTEXT_CSS_Origin origin;        ///< Its origin.
  GTEXT_CSS_Layer_Id parent;      ///< The layer it is nested in (GTEXT_CSS_LAYER_NONE at the top level).
  const char * segment;           ///< Its own last segment; NULL for an anonymous one. Owned by the registry.
  size_t segment_length;          ///< Its length.
  bool anonymous;                 ///< An anonymous segment.
  size_t depth;                   ///< 1 at the top level.
  size_t rank;                    ///< Frozen: its place in the order of its origin, from 0; the unlayered styles are gtext_css_layer_count() (after every layer). Else 0.
} GTEXT_CSS_Layer_Info;

/** @brief The information of a layer. @return false for an id the registry does not have (and `out` is zeroed). */
GTEXT_API bool gtext_css_layer_info(const GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Layer_Id layer, GTEXT_CSS_Layer_Info * out);

/**
 * @brief The rank of a layer of an origin (GTEXT_CSS_LAYER_NONE: the unlayered styles, which are last).
 *
 * @param registry A frozen registry. Not NULL.
 * @param origin The origin.
 * @param layer The layer, which must be of that origin.
 * @param rank Receives it. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (not frozen, a layer the registry lacks or one of another origin, an origin without layers).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_layer_rank(const GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id layer,
    size_t * rank);

/**
 * @brief Compare two layers of an origin: -1 if `a` comes first (is weaker in a normal declaration), 0 if they are one, 1 if `a` comes after `b`.
 *
 * @param order Receives -1, 0 or 1. Not NULL.
 * @return As gtext_css_layer_rank().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_layer_compare(const GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id a,
    GTEXT_CSS_Layer_Id b, int * order);

/**
 * @brief Write the dotted path of a layer (`framework.theme`); a `.` or `\\` in a segment is escaped with a `\\`; an anonymous segment is written `(anonymous)`;
 *   GTEXT_CSS_LAYER_NONE is the empty string.
 *
 * @param registry The registry. Not NULL.
 * @param layer The layer.
 * @param buffer Where to write, NUL-terminated if it fits; may be NULL with a `capacity` of 0.
 * @param capacity Its size.
 * @return The length of the whole path (as snprintf): more than `capacity - 1` means the text was cut; 0 for an id the registry lacks.
 */
GTEXT_API size_t gtext_css_layer_path(const GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Layer_Id layer, char * buffer, size_t capacity);

/* ---- reading the layers of a sheet ------------------------------------------------------------------------------ */

/**
 * @brief Whether a conditional rule (`@media`, `@supports`, `@container`, `@document`) applies. Return true to read the layers inside it.
 *
 * Cascade 5, 6.4.3: "Layers that are defined inside of a conditional group rule do not contribute to the layer order unless the condition is
 * true or unless the conditional group rule can evaluate differently for different elements in the document." A rule whose condition depends on
 * the element (`@container`) must be answered true. The gate that evaluates conditions is C4's; the structure is final here.
 */
typedef bool (*GTEXT_CSS_Layer_Active_Fn)(void * user, const GTEXT_CSS_Rule * rule);

/** @brief Called for each `@layer` block and each `@import` with a layer, with the layer its contents or its sheet belong in. */
typedef void (*GTEXT_CSS_Layer_Rule_Fn)(void * user, const GTEXT_CSS_Rule * rule, GTEXT_CSS_Layer_Id layer);

/** @brief What reading the layers of a sheet may ask of the caller. A zeroed struct is the default. */
typedef struct {
  void * user;                          ///< Passed to both callbacks.
  GTEXT_CSS_Layer_Active_Fn active;     ///< NULL: a conditional rule that holds a layer is GTEXT_CSS_E_UNSUPPORTED (naming `active_rule`), never a guess; one that holds none is skipped.
  GTEXT_CSS_Layer_Rule_Fn on_layer;     ///< NULL: none.
  /** Called once for every layer a rule declares or finds again, in the order the rules are read: each name of a statement, a block's layer, an import's: the **occurrences**
   *  of layers, where `on_layer` is called for blocks and imports only and a layer already declared is not new. A sheet that keeps its conditions as data needs every one of them, because
   *  a layer first named in a rule that does not apply is declared by the next rule that names it and does. NULL: none. */
  GTEXT_CSS_Layer_Rule_Fn on_declare;
  size_t max_rule_depth;                ///< Deepest nesting of rules read. Default 4,096; at most 1,048,576. One more is GTEXT_CSS_E_LIMIT_DEPTH.
} GTEXT_CSS_Layer_Collect_Options;

/**
 * @brief Declare the layers a parsed sheet declares, in the order they appear.
 *
 * Reads, without recursion, the `@layer` statements (`@layer a, b.c;`), the `@layer` blocks (named or anonymous, nested in each other and in
 * conditional group rules, `@scope`, `@starting-style` and style rules) and `@import` rules with `layer` or `layer(name)`. A rule that is
 * invalid by Cascade 5 (a CSS-wide keyword as a name, whitespace inside a name, an empty statement, two names on a block) is skipped as the
 * standard says an invalid rule is, with its contents, and declares nothing. Layers already declared are found, not duplicated.
 *
 * @param registry The registry, not frozen. Not NULL.
 * @param origin The origin of the sheet.
 * @param base The layer the sheet is imported into (GTEXT_CSS_LAYER_NONE for a top-level sheet): "a layered @import inside a layered import" nests.
 * @param tree A tree of a stylesheet (or of block contents). Not NULL.
 * @param options NULL for the defaults.
 * @param result NULL, or filled; on a failure its `span` is the rule concerned.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED (see `active`); GTEXT_CSS_E_LIMIT_LAYERS; GTEXT_CSS_E_LIMIT_LAYER_DEPTH;
 *   GTEXT_CSS_E_LIMIT_DEPTH; GTEXT_CSS_E_OOM. The layers declared before a failure remain.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_cascade_collect_layers(GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id base,
    const GTEXT_CSS_Tree * tree, const GTEXT_CSS_Layer_Collect_Options * options, GTEXT_CSS_Layer_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_LAYER_H
