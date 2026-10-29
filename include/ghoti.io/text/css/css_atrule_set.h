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
 * @file css_atrule_set.h
 * @brief The at-rules of a document's style sheets, collected in cascade order and found by name: font faces, feature values, palettes, color profiles, counter styles, keyframes,
 *   view transitions, position-try rules and pages.
 *
 * Normative text: CSS Cascading 5 (6.4 "Cascade Layers": a later layer wins; of two rules in one layer the later wins; names are scoped to their context), CSS Fonts 4 (4.3: "the later
 * rule wins" for one family and descriptor set), CSS Animations 1 (3: "If multiple @keyframes rules are defined with the same name, the last one in the highest layer wins"), CSS Counter
 * Styles 3 (2: "if multiple @counter-style rules define the same name, the last one wins"), CSS View Transitions 2 (`@view-transition`: "the last"), CSS Anchor Positioning 1
 * (`@position-try`: "last wins") and CSS Paged Media 3 (4.4, the page cascade), as served 2026-10-29.
 *
 * Make a set with gtext_css_atrule_set_new(), give it each sheet's tree in the order of the document with gtext_css_atrule_set_add_tree() (a conditional group rule holds when the set's
 * conditions say so; `@layer` orders the rules), call gtext_css_atrule_set_freeze(), and ask. Everything the finds return belongs to the set.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_ATRULE_SET_H
#define GHOTI_IO_GTEXT_CSS_CSS_ATRULE_SET_H

#include <ghoti.io/text/css/css_atrule.h>
#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_counter_style.h>
#include <ghoti.io/text/css/css_font_rules.h>
#include <ghoti.io/text/css/css_keyframes.h>
#include <ghoti.io/text/css/css_layer.h>
#include <ghoti.io/text/css/css_page.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A set of at-rules. */
typedef struct GTEXT_CSS_Atrule_Set GTEXT_CSS_Atrule_Set;

/** @brief Options. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;     ///< NULL for the registry's.
  const GTEXT_CSS_Conditions * conditions; ///< NULL: a conditional group rule that holds an at-rule the set collects is GTEXT_CSS_E_UNSUPPORTED (never a guess).
  GTEXT_CSS_Origin origin;               ///< The origin of the sheets (author by default).
  uint32_t context;                      ///< The tree-scoped context (the document is 0; each shadow tree has its own): names are found within one context.
  size_t max_rules;                      ///< Most at-rules the set holds. Default 1,000,000. One more is GTEXT_CSS_E_LIMIT_DESCRIPTORS.
  size_t max_rule_depth;                 ///< Deepest nesting of rules read. Default 4,096. One more is GTEXT_CSS_E_LIMIT_DEPTH.
  GTEXT_CSS_Atrule_Options atrule;       ///< How each rule is read (zeroed: the defaults).
} GTEXT_CSS_Atrule_Set_Options;

/** @brief The defaults. */
GTEXT_API GTEXT_CSS_Atrule_Set_Options gtext_css_atrule_set_options_default(void);

/** @brief Make a set. @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM. */
GTEXT_API GTEXT_CSS_Status gtext_css_atrule_set_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Atrule_Set_Options * options, GTEXT_CSS_Atrule_Set ** out);

/** @brief Release a set and everything it holds; NULL is ignored. */
GTEXT_API void gtext_css_atrule_set_free(GTEXT_CSS_Atrule_Set * set);

/**
 * @brief Collect the at-rules of a tree, in document order. Rules the pin recognizes but the set does not collect are ignored; an at-rule invalid by the registry is dropped.
 *
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a frozen set); GTEXT_CSS_E_UNSUPPORTED (see `conditions`); GTEXT_CSS_E_LIMIT_DEPTH; GTEXT_CSS_E_LIMIT_DESCRIPTORS; GTEXT_CSS_E_LIMIT_LAYERS; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_atrule_set_add_tree(GTEXT_CSS_Atrule_Set * set, const GTEXT_CSS_Tree * tree);

/** @brief Fix the layer ranks and the order; adds are then refused and the finds work. Idempotent. */
GTEXT_API GTEXT_CSS_Status gtext_css_atrule_set_freeze(GTEXT_CSS_Atrule_Set * set);

/** @brief One collected at-rule. */
typedef struct {
  const GTEXT_CSS_Atrule * rule;  ///< Borrowed: it lives as long as the set.
  GTEXT_CSS_Layer_Id layer;       ///< The layer the rule is in (GTEXT_CSS_LAYER_NONE: unlayered).
  size_t layer_rank;              ///< Its rank (the unlayered rules are last). Valid after freeze.
  size_t order;                   ///< The place among the collected rules (document order, across trees).
  size_t tree;                    ///< Which tree: 0 for the first given.
} GTEXT_CSS_Atrule_Entry;

/** @brief How many rules the set holds (all kinds). */
GTEXT_API size_t gtext_css_atrule_set_count(const GTEXT_CSS_Atrule_Set * set);

/** @brief Entry `index` in cascade order (a later layer, then a later place, is later). False: out of range or not frozen. */
GTEXT_API bool gtext_css_atrule_set_entry(const GTEXT_CSS_Atrule_Set * set, size_t index, GTEXT_CSS_Atrule_Entry * out);

/** @brief The layer registry the set declared (frozen by gtext_css_atrule_set_freeze()). */
GTEXT_API const GTEXT_CSS_Layer_Registry * gtext_css_atrule_set_layers(const GTEXT_CSS_Atrule_Set * set);

/**
 * @brief The defined faces of a family, in cascade order (the last wins where descriptors coincide), as many as `capacity`; the return is the total. A family key is the folded name
 *   (GTEXT_CSS_Font_Face::family_key); gtext_css_atrule_set_family_key() makes one.
 */
GTEXT_API size_t gtext_css_atrule_set_font_faces(const GTEXT_CSS_Atrule_Set * set, const char * family_key, size_t key_length, const GTEXT_CSS_Font_Face ** out, size_t capacity);

/** @brief The folded key of a family name for the finds (written into `buffer`; the return is the length, 0 on a failure). */
GTEXT_API size_t gtext_css_atrule_set_family_key(const char * family, size_t length, char * buffer, size_t capacity);

/**
 * @brief The `@font-feature-values` of a family merged in cascade order: for each feature type, a later name replaces an earlier one. The result belongs to the set. NULL: none.
 */
GTEXT_API const GTEXT_CSS_Feature_Values * gtext_css_atrule_set_feature_values(const GTEXT_CSS_Atrule_Set * set, const char * family_key, size_t key_length);

/** @brief The `@font-palette-values` with this dashed name for this family (the last in the highest layer wins). NULL: none. */
GTEXT_API const GTEXT_CSS_Palette_Values * gtext_css_atrule_set_palette_values(const GTEXT_CSS_Atrule_Set * set, const char * name, size_t name_length, const char * family_key,
    size_t key_length);

/** @brief The `@color-profile` of this name (the last wins). NULL: none. */
GTEXT_API const GTEXT_CSS_Color_Profile * gtext_css_atrule_set_color_profile(const GTEXT_CSS_Atrule_Set * set, const char * name, size_t length);

/** @brief The `@counter-style` of this name (the last in the highest layer wins), its `extends` and `fallback` chains resolved. NULL: none. */
GTEXT_API const GTEXT_CSS_Counter_Style * gtext_css_atrule_set_counter_style(const GTEXT_CSS_Atrule_Set * set, const char * name, size_t length);

/** @brief The `@keyframes` of this name (the last in the highest layer wins). NULL: none. */
GTEXT_API const GTEXT_CSS_Keyframes * gtext_css_atrule_set_keyframes(const GTEXT_CSS_Atrule_Set * set, const char * name, size_t length);

/** @brief The applicable `@view-transition` (the last in cascade order). NULL: none. */
GTEXT_API const GTEXT_CSS_Atrule * gtext_css_atrule_set_view_transition(const GTEXT_CSS_Atrule_Set * set);

/** @brief The `@position-try` of this dashed name (the last in the highest layer wins). NULL: none. */
GTEXT_API const GTEXT_CSS_Atrule * gtext_css_atrule_set_position_try(const GTEXT_CSS_Atrule_Set * set, const char * name, size_t length);

/** @brief One `@page` rule that matches a page. */
typedef struct {
  const GTEXT_CSS_Atrule * rule;
  GTEXT_CSS_Page_Specificity specificity;  ///< The highest of the selectors that match.
  GTEXT_CSS_Layer_Id layer;
  size_t layer_rank;
  size_t order;
} GTEXT_CSS_Page_Match;

/**
 * @brief The `@page` rules that match a page, in the order the page cascade applies them: layer rank, then specificity (f, g, h), then place; the later wins. The return is the total.
 *   Its margin rules and declarations are on the rule (child rules of kind GTEXT_CSS_ATRULE_PAGE_MARGIN, descriptors).
 */
GTEXT_API size_t gtext_css_atrule_set_page_collect(const GTEXT_CSS_Atrule_Set * set, const GTEXT_CSS_Page_Context * page, GTEXT_CSS_Page_Match * out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_ATRULE_SET_H
