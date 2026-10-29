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
 * @file css_atrule.h
 * @brief The typed model of the at-rules that carry data: `@font-face`, `@font-feature-values`, `@font-palette-values`, `@color-profile`, `@counter-style`, `@page` and its
 *   margin rules, `@keyframes`, `@view-transition`, `@position-try`, `@namespace`, `@import`, `@charset`, and the ones that are recognized and inactive.
 *
 * Normative text: CSS Fonts 4 and 5, CSS Color 5, CSS Counter Styles 3, CSS Paged Media 3, CSS Animations 1 and 2, CSS View Transitions 1 and 2, CSS Anchor Positioning 1 and 2,
 * CSS Namespaces 3, CSSOM (serialize a CSS rule), as served 2026-10-29.
 *
 * ## The model
 *
 * gtext_css_atrule_parse() turns one parsed at-rule (css_core.h) into an immutable, atomically reference counted @ref GTEXT_CSS_Atrule of a @ref GTEXT_CSS_Atrule_Kind, which keeps
 * the prelude **typed** (the names, the page selectors, the keyframe offsets) and the **descriptors typed** (each is validated by the grammar of the registry, which already holds
 * the pin's descriptors, through the matcher and the value builder the properties use), and gtext_css_atrule_serialize() writes it as CSSOM's "serialize a CSS rule" does. The
 * typed structs of css_font_rules.h, css_counter_style.h, css_page.h and css_keyframes.h are read from it.
 *
 * ## Error handling
 *
 * **An invalid descriptor is dropped with a diagnostic and the rule stands** (Fonts 4: an invalid descriptor is ignored), an unknown descriptor likewise. A rule is **ignored as a
 * whole** only where the served text says so (`@font-face` without `font-family` or `src`; `@font-palette-values` without `font-family`; an invalid prelude; an `@namespace` after a
 * style rule): gtext_css_atrule_parse() then returns OK with no rule and `result->valid` false, and the diagnostics say why. A rule that is valid and defines nothing (a
 * `@counter-style` whose `symbols` are too few for its system) is kept, flagged. The tables are data: tools/css/atrule_required.tsv (the required descriptors) and
 * tools/css/atrule_parents.tsv (where each at-rule may be), read by tools/css/gen-atrule-tables.py.
 *
 * ## Limits
 *
 * `max_descriptors` of a rule, `max_children` (keyframe blocks, margin rules, nested at-rules); one more is GTEXT_CSS_E_LIMIT_DESCRIPTORS. Nothing recurses.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_ATRULE_H
#define GHOTI_IO_GTEXT_CSS_CSS_ATRULE_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What an at-rule is, for the model. */
typedef enum {
  GTEXT_CSS_ATRULE_UNKNOWN = 0,   ///< Not an at-rule of the pin: its contents never apply.
  GTEXT_CSS_ATRULE_CHARSET,       ///< `@charset` (the decoder has read the bytes by it).
  GTEXT_CSS_ATRULE_IMPORT,        ///< `@import`: the typed request.
  GTEXT_CSS_ATRULE_NAMESPACE,     ///< `@namespace`.
  GTEXT_CSS_ATRULE_LAYER,         ///< `@layer` (names or a block): the model keeps the prelude.
  GTEXT_CSS_ATRULE_SCOPE,         ///< `@scope`.
  GTEXT_CSS_ATRULE_PROPERTY,      ///< `@property`: its descriptors; css_at_property.h applies it.
  GTEXT_CSS_ATRULE_MEDIA,         ///< `@media`.
  GTEXT_CSS_ATRULE_SUPPORTS,      ///< `@supports`.
  GTEXT_CSS_ATRULE_CONTAINER,     ///< `@container`.
  GTEXT_CSS_ATRULE_STARTING_STYLE, ///< `@starting-style`.
  GTEXT_CSS_ATRULE_WHEN,          ///< `@when`.
  GTEXT_CSS_ATRULE_ELSE,          ///< `@else`.
  GTEXT_CSS_ATRULE_CUSTOM_MEDIA,  ///< `@custom-media`.
  GTEXT_CSS_ATRULE_FONT_FACE,     ///< `@font-face`.
  GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES, ///< `@font-feature-values`: family names, and nested feature blocks.
  GTEXT_CSS_ATRULE_FEATURE_BLOCK, ///< `@stylistic`, `@historical-forms`, `@styleset`, `@character-variant`, `@swash`, `@ornaments`, `@annotation`: name to integers.
  GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES, ///< `@font-palette-values`.
  GTEXT_CSS_ATRULE_COLOR_PROFILE, ///< `@color-profile`.
  GTEXT_CSS_ATRULE_COUNTER_STYLE, ///< `@counter-style`.
  GTEXT_CSS_ATRULE_PAGE,          ///< `@page`: selectors, descriptors, page properties, margin rules.
  GTEXT_CSS_ATRULE_PAGE_MARGIN,   ///< `@top-left` ... `@right-bottom`: a margin rule of a page (its name is the box).
  GTEXT_CSS_ATRULE_KEYFRAMES,     ///< `@keyframes` (and `@-webkit-keyframes`).
  GTEXT_CSS_ATRULE_KEYFRAME,      ///< One block of `@keyframes`: its offsets and its declarations.
  GTEXT_CSS_ATRULE_VIEW_TRANSITION, ///< `@view-transition`.
  GTEXT_CSS_ATRULE_POSITION_TRY,  ///< `@position-try`.
  GTEXT_CSS_ATRULE_PRESERVED,     ///< Recognized and inactive: `@function`, `@mixin`, `@contents`, `@apply`, `@private`, `@custom-selector`, `@navigation`, `@location`, `@supports-condition`.
  GTEXT_CSS_ATRULE_KIND_COUNT     ///< The number of kinds; not a kind.
} GTEXT_CSS_Atrule_Kind;

/** @brief The name of a kind (`font-face`, `page-margin`, ...). Static. */
GTEXT_API const char * gtext_css_atrule_kind_name(GTEXT_CSS_Atrule_Kind kind);

/** @brief A parsed at-rule (the struct is private). Immutable, reference counted, shareable between threads. */
typedef struct GTEXT_CSS_Atrule GTEXT_CSS_Atrule;

/** @brief Where an at-rule is read: a rule may be only in some places (`@font-face` may not be inside a style rule). */
typedef enum {
  GTEXT_CSS_ATRULE_PARENT_TOP = 0,        ///< The top level of a style sheet.
  GTEXT_CSS_ATRULE_PARENT_GROUP,          ///< Inside a conditional group rule, `@layer` or `@scope`.
  GTEXT_CSS_ATRULE_PARENT_STYLE,          ///< Inside a style rule.
  GTEXT_CSS_ATRULE_PARENT_PAGE,           ///< Inside `@page` (the margin rules).
  GTEXT_CSS_ATRULE_PARENT_FEATURE_VALUES, ///< Inside `@font-feature-values` (the feature blocks).
  GTEXT_CSS_ATRULE_PARENT_MIXIN           ///< Inside a mixin.
} GTEXT_CSS_Atrule_Parent;

/** @brief What parsing an at-rule may be told. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the registry's.
  bool quirks;                        ///< Declarations (keyframes, page, position-try) are read in quirks mode (css_property.h).
  size_t max_descriptors;             ///< Most descriptors and declarations of one rule. Default 4,096. One more is GTEXT_CSS_E_LIMIT_DESCRIPTORS.
  size_t max_children;                ///< Most nested rules (keyframe blocks, margin rules, feature blocks) of one rule. Default 65,536. One more is GTEXT_CSS_E_LIMIT_DESCRIPTORS.
  size_t max_diagnostics;             ///< Most diagnostics kept (the rest are counted). Default 1,024.
  size_t max_list;                    ///< Most entries of a list a rule's prelude or a descriptor holds that the model unfolds (`src` entries, keyframe selectors, unicode ranges, feature settings). Default 65,536.
  GTEXT_CSS_Atrule_Parent parent;     ///< Where the rule is read (the default is the top level of a style sheet); a rule the place does not allow is invalid.
} GTEXT_CSS_Atrule_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Atrule_Options gtext_css_atrule_options_default(void);

/** @brief What was dropped, and why. */
typedef enum {
  GTEXT_CSS_ATRULE_DIAG_UNKNOWN_DESCRIPTOR = 0, ///< A descriptor the at-rule does not have: dropped.
  GTEXT_CSS_ATRULE_DIAG_INVALID_DESCRIPTOR,     ///< A descriptor whose value is not valid for it: dropped.
  GTEXT_CSS_ATRULE_DIAG_UNKNOWN_PROPERTY,       ///< A declaration of a property the registry does not know (a keyframe, a page): dropped.
  GTEXT_CSS_ATRULE_DIAG_INVALID_VALUE,          ///< A declaration whose value is not valid for its property: dropped.
  GTEXT_CSS_ATRULE_DIAG_NOT_ALLOWED,            ///< A declaration the at-rule does not take (`color` in `@position-try`; `animation-name` in a keyframe): dropped.
  GTEXT_CSS_ATRULE_DIAG_IMPORTANT_IGNORED,      ///< `!important` in a keyframe: the declaration is ignored (Animations 1).
  GTEXT_CSS_ATRULE_DIAG_INVALID_RULE,           ///< The prelude or the place is not valid: the rule is ignored.
  GTEXT_CSS_ATRULE_DIAG_MISSING_REQUIRED,       ///< A descriptor the rule requires is missing: the rule is ignored.
  GTEXT_CSS_ATRULE_DIAG_NOT_DEFINED,            ///< Valid, and it defines nothing (a `@counter-style` with too few symbols).
  GTEXT_CSS_ATRULE_DIAG_NESTED_RULE             ///< A nested rule the at-rule does not take, or an invalid keyframe selector: dropped.
} GTEXT_CSS_Atrule_Diag_Kind;

/** @brief A descriptor, declaration, nested rule or rule that was dropped. */
typedef struct {
  GTEXT_CSS_Atrule_Diag_Kind kind;
  GTEXT_CSS_Span span;
  const char * reference;  ///< The sentence of the draft that decides it (static, never NULL).
} GTEXT_CSS_Atrule_Diagnostic;

/** @brief What parsing reports. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;  ///< What the call returned.
  const char * message;     ///< A static description; never NULL after a call.
  GTEXT_CSS_Span span;      ///< A limit: where.
  char name[96];            ///< What is concerned, cut to fit.
  bool valid;               ///< The rule stands (a rule is returned). False: it is ignored as a whole; see the diagnostics of the call.
  size_t diagnostic_total;  ///< Every diagnostic raised, kept or not.
  size_t diagnostic_count;  ///< Those the call wrote into `diagnostics` (up to its capacity).
} GTEXT_CSS_Atrule_Result;

/**
 * @brief Parse one at-rule of a tree (css_core.h) into the model.
 *
 * @param registry The registry (the descriptors and their grammars). Not NULL; it must outlive the rule.
 * @param rule An at-rule (a nested keyframe block is a qualified rule: it is read with its `@keyframes`). Not NULL.
 * @param options NULL for the defaults.
 * @param out Receives the rule (one owner), or NULL when the rule is ignored. Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK (also for a rule that is ignored: `result->valid` is false); GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_DESCRIPTORS and the limits of values.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_atrule_parse(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Rule * rule, const GTEXT_CSS_Atrule_Options * options,
    GTEXT_CSS_Atrule ** out, GTEXT_CSS_Atrule_Result * result);

/** @brief Another owner (the rule is freed when the last lets go). Returns `rule`; NULL is ignored. */
GTEXT_API GTEXT_CSS_Atrule * gtext_css_atrule_retain(const GTEXT_CSS_Atrule * rule);

/** @brief One owner fewer; the last frees the rule. NULL is ignored. */
GTEXT_API void gtext_css_atrule_free(const GTEXT_CSS_Atrule * rule);

/** @brief The kind of the rule. */
GTEXT_API GTEXT_CSS_Atrule_Kind gtext_css_atrule_kind(const GTEXT_CSS_Atrule * rule);

/** @brief The name of the rule as the registry has it, lower case, without the `@` (`font-face`, `top-left`, `webkit-keyframes` for the alias is `keyframes`); valid for the life of the rule. */
GTEXT_API const char * gtext_css_atrule_name(const GTEXT_CSS_Atrule * rule, size_t * length);

/** @brief The span of the rule in its input. */
GTEXT_API GTEXT_CSS_Span gtext_css_atrule_span(const GTEXT_CSS_Atrule * rule);

/** @brief The prelude as canonical text (`card`, `:first`, `"A", B`); the empty string for a rule with none. Valid for the life of the rule. */
GTEXT_API const char * gtext_css_atrule_prelude_text(const GTEXT_CSS_Atrule * rule, size_t * length);

/** @brief The names of the prelude, decoded, in order: the family names of `@font-feature-values` and `@font-palette-values`, the name of `@counter-style`, `@keyframes`, `@position-try`, `@color-profile`, the namespace prefix, the layer names. */
GTEXT_API size_t gtext_css_atrule_name_count(const GTEXT_CSS_Atrule * rule);
GTEXT_API bool gtext_css_atrule_name_at(const GTEXT_CSS_Atrule * rule, size_t index, GTEXT_CSS_String * out, bool * quoted);

/** @brief One descriptor or declaration of the rule. The strings and the value are owned by the rule. */
typedef struct {
  const char * name;                 ///< The name, lower case (a custom property keeps its case).
  size_t name_length;
  GTEXT_CSS_Descriptor_Id descriptor; ///< The descriptor of the registry (0 for a property declaration or a custom property).
  bool property;                     ///< A declaration of a property (of a keyframe, a page, `@position-try`), not a descriptor.
  GTEXT_CSS_Property_Id property_id; ///< The property (0 for a descriptor or a custom property).
  bool important;                    ///< Written `!important`.
  const GTEXT_CSS_Value_Set * set;   ///< The typed value (NULL for a custom property).
  GTEXT_CSS_String text;             ///< The value as canonical text (always set).
  GTEXT_CSS_Id declaration;          ///< The id of the declaration in the syntax tree.
  GTEXT_CSS_Span span;
} GTEXT_CSS_Atrule_Descriptor;

/** @brief How many valid descriptors and declarations the rule holds, in the order written (a later one of a name replaces the earlier and takes its place at the end, as CSSOM's declaration block does). */
GTEXT_API size_t gtext_css_atrule_descriptor_count(const GTEXT_CSS_Atrule * rule);

/** @brief The `index`th; false past the end. */
GTEXT_API bool gtext_css_atrule_descriptor_at(const GTEXT_CSS_Atrule * rule, size_t index, GTEXT_CSS_Atrule_Descriptor * out);

/** @brief The descriptor or declaration with this name (ASCII case-insensitive for descriptors and properties); false if the rule has none. */
GTEXT_API bool gtext_css_atrule_descriptor_find(const GTEXT_CSS_Atrule * rule, const char * name, size_t length, GTEXT_CSS_Atrule_Descriptor * out);

/** @brief The nested rules (keyframe blocks, margin rules, feature blocks), in order, borrowed from the rule. */
GTEXT_API size_t gtext_css_atrule_child_count(const GTEXT_CSS_Atrule * rule);
GTEXT_API const GTEXT_CSS_Atrule * gtext_css_atrule_child_at(const GTEXT_CSS_Atrule * rule, size_t index);

/** @brief Valid, and it defines nothing (a `@counter-style` with too few symbols for its system): see the diagnostics. */
GTEXT_API bool gtext_css_atrule_defines_nothing(const GTEXT_CSS_Atrule * rule);

/** @brief The diagnostics the parse of the rule kept, and how many it raised. */
GTEXT_API size_t gtext_css_atrule_diagnostic_total(const GTEXT_CSS_Atrule * rule);
GTEXT_API bool gtext_css_atrule_diagnostic(const GTEXT_CSS_Atrule * rule, size_t index, GTEXT_CSS_Atrule_Diagnostic * out);

/**
 * @brief CSSOM's "serialize a CSS rule" for the rule (its `cssText`): `@font-face { font-family: "A"; src: url("a.woff2") format("woff2"); }`, with the descriptors in order and
 *   each value as the registry serializes it. Parse, serialize, parse is a fixed point.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_atrule_serialize(const GTEXT_CSS_Atrule * rule, const GTEXT_Markup_Sink * sink);

/** @brief gtext_css_atrule_serialize() to a new NUL-terminated buffer; free it with gtext_css_atrule_buffer_free(). */
GTEXT_API GTEXT_CSS_Status gtext_css_atrule_serialize_buffer(const GTEXT_CSS_Atrule * rule, const GTEXT_Allocator * allocator, char ** out, size_t * length);

/** @brief Release a buffer from gtext_css_atrule_serialize_buffer(). */
GTEXT_API void gtext_css_atrule_buffer_free(const GTEXT_Allocator * allocator, char * buffer);

/**
 * @brief Parse one descriptor value through the registry: the grammar of the descriptor, the matcher and the value builder the properties use.
 *
 * @param registry The registry. Not NULL.
 * @param descriptor A descriptor of the registry.
 * @param values The component values of the value. NULL only with a count of 0.
 * @param count Their number.
 * @param allocator NULL for the registry's.
 * @param set Receives the typed value (one owner) when the value is valid, else NULL. Not NULL.
 * @return GTEXT_CSS_OK (valid or not: `*set` says); GTEXT_CSS_E_INVALID (no such descriptor, or it has no grammar); GTEXT_CSS_E_OOM and the limits of values.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_descriptor_parse(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Descriptor_Id descriptor, const GTEXT_CSS_Component_Value * values,
    size_t count, const GTEXT_Allocator * allocator, GTEXT_CSS_Value_Set ** set);

/**
 * @struct GTEXT_CSS_Atrule_Validity
 * @brief The standard's "valid in the current context" for at-rules, to hand to a parse (GTEXT_CSS_Parse_Options::validity).
 *
 * "Return the rule if it is valid in the current context": a rule of a kind this model knows is valid when gtext_css_atrule_parse() says so in the place it was read (its parent);
 * every other construct, and every at-rule the pin does not list, is left valid (they are the generic tree's). A parse with the callback keeps exactly the at-rules a parse without it
 * keeps when every one is valid; an invalid one is dropped as the standard says an invalid rule is. The struct must outlive the parse.
 */
typedef struct {
  GTEXT_CSS_Validity validity;           ///< Pass this as the parse options' `validity`.
  const GTEXT_CSS_Registry * registry;   ///< Private to the callback.
  size_t checked;                        ///< At-rules the callback judged.
  size_t refused;                        ///< ... and found invalid.
} GTEXT_CSS_Atrule_Validity;

/** @brief Fill a validity for a registry. `registry` must outlive it. */
GTEXT_API void gtext_css_atrule_validity_init(GTEXT_CSS_Atrule_Validity * validity, const GTEXT_CSS_Registry * registry);

/** @brief The number of rows of the at-rule ledger (tools/css/atrule_depth.tsv): the at-rules of the pin. */
GTEXT_API size_t gtext_css_atrule_ledger_count(void);

/** @brief Row `index` of the ledger: the at-rule's name, and what the library does with it (`applies`, `typed`, `serializes`, `preserved`). false past the end. */
GTEXT_API bool gtext_css_atrule_ledger_row(size_t index, const char ** name, bool * parses, bool * validates, bool * serializes, bool * applies, bool * typed, bool * preserved);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_ATRULE_H
