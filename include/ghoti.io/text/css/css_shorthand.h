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
 * @file css_shorthand.h
 * @brief Shorthands and declaration blocks: expansion of a shorthand's typed value into longhands, and CSSOM's serialization of a
 *   block back into the shortest shorthands.
 *
 * Reference: CSS Cascade Level 5, "Shorthand properties" and "Resolving the CSS-wide keywords"; CSS Variables Level 1 (a shorthand with
 * `var()` sets every longhand to a pending-substitution value); CSSOM, "serialize a CSS declaration block" and "serialize a CSS value"
 * (https://drafts.csswg.org/cssom/, served 2026-10-26).
 *
 * ## Expansion
 *
 * gtext_css_shorthand_expand() takes what gtext_css_parse_property() returned for a shorthand and makes a value for each longhand and each
 * property the shorthand resets (the pin's `resetLonghands`: `font` also sets `font-kerning` to its initial value). A shorthand is split by
 * its **kind** (gtext_css_shorthand_kind(), tools/css/shorthand_rules.tsv):
 *
 * - BOX: one to four values, distributed top, right, bottom, left (the pin's longhand order) as CSS 2 has it;
 * - PAIR: one or two values, a missing second being the first;
 * - ANY: components in any order. Each component is assigned to the longhand whose grammar accepts it (the value is split into the longhands'
 *   own texts and each is parsed by its own grammar, so a longhand's value is exactly what writing it would have given); a longhand with no
 *   component is reset to its initial value. A `/` separates the components of two longhands that follow each other in the pin's order;
 * - LAYERS: comma separated layers, each split as ANY; the longhands are comma lists, the last layer alone carrying the property the grammar
 *   gives only to the final layer (`background-color`); a longhand with no component in a layer gets its initial value for that layer;
 * - COPY: every longhand takes the whole value;
 * - ALL: `all` takes the CSS-wide keywords alone;
 * - BORDER_RADIUS (the four corners, or the two of one side), FONT, FLEX, GRID_LINE, GRID_TEMPLATE, GRID, OFFSET, TEXT_ALIGN, FONT_SYNTHESIS: the shorthands whose grammar the
 *   generic split would misread have algorithms of their own.
 *
 * A CSS-wide keyword sets every longhand to that keyword. A **pending** value (a shorthand holding `var()`) sets every longhand to a pending
 * value that remembers the shorthand's own text, so that serialization can write the shorthand back. A shorthand of the pin with no rule
 * (the kind UNIMPLEMENTED) is GTEXT_CSS_E_UNSUPPORTED naming it; nothing is guessed.
 *
 * A longhand of a shorthand may itself be a shorthand (`border` sets `border-width`, which sets four sides). Expansion goes all the way
 * down: the list holds longhands only.
 *
 * ## Declaration blocks
 *
 * A @ref GTEXT_CSS_Declaration_Block is an ordered list of declarations: a style attribute, a rule's block, or the winning declarations
 * the cascade hands to the computed-value layer in ascending priority (a later declaration beats an earlier one for the same property). A shorthand added
 * to a block is expanded into its longhands, each with the shorthand's importance. The block keeps values as typed sets (retained, so the
 * caller's result may be freed after the call) and is not safe to share while it is changed; a finished block may be read by any number of threads.
 *
 * Serializing a block follows CSSOM: for each declaration in order, if its longhand belongs to a shorthand whose longhands are all in the block
 * with equal importance and none pending, the declaration is written once as the shortest form of the shorthand
 * (gtext_css_block_shorthand_value()) and its longhands are not written again; otherwise as `name: value`. A shorthand is tried in the order
 * of most longhands first (the one that covers the most of the block wins a longhand that two of them contain; ties go to the registry's order).
 * WPT's shorthand serialization vectors are scored against it (make conformance-css-property). A shorthand is written only if the text, read back as that
 * shorthand, gives every longhand the value the block holds (`box-shadow-offset: none` beside a color has no text in `box-shadow`, and the longhands are
 * written one by one).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_SHORTHAND_H
#define GHOTI_IO_GTEXT_CSS_CSS_SHORTHAND_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/css/css_value.h>
#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/markup/markup_escape.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How a shorthand is split (tools/css/shorthand_rules.tsv; the generator reads the enumerators from this header, in this order).
 */
typedef enum {
  GTEXT_CSS_SHORTHAND_UNIMPLEMENTED = 0, ///< No rule: expansion is GTEXT_CSS_E_UNSUPPORTED.
  GTEXT_CSS_SHORTHAND_BOX,               ///< `{1,4}` over four longhands.
  GTEXT_CSS_SHORTHAND_PAIR,              ///< One or two values over two longhands.
  GTEXT_CSS_SHORTHAND_ANY,               ///< Components in any order, each to the longhand that accepts it.
  GTEXT_CSS_SHORTHAND_LAYERS,            ///< Comma separated layers of ANY.
  GTEXT_CSS_SHORTHAND_COPY,              ///< The whole value to every longhand.
  GTEXT_CSS_SHORTHAND_ALL,               ///< `all`: the CSS-wide keywords alone.
  GTEXT_CSS_SHORTHAND_BORDER_RADIUS,     ///< `border-radius`: one to four radii, a slash, one to four.
  GTEXT_CSS_SHORTHAND_FONT,              ///< `font`.
  GTEXT_CSS_SHORTHAND_FLEX,              ///< `flex`.
  GTEXT_CSS_SHORTHAND_GRID_LINE,         ///< `grid-row`, `grid-column`, `grid-area`: `<grid-line>` values separated by `/`, an omitted one copying an identifier or being `auto`.
  GTEXT_CSS_SHORTHAND_TEXT_ALIGN,        ///< `text-align`: `text-align-all`, and `text-align-last` `auto` (or `justify` for `justify-all`).
  GTEXT_CSS_SHORTHAND_FONT_SYNTHESIS,    ///< `font-synthesis`: `none`, or the listed kinds `auto` and the others `none`.
  GTEXT_CSS_SHORTHAND_GRID_TEMPLATE,     ///< `grid-template`: `none`, rows `/` columns, or area strings with their row sizes.
  GTEXT_CSS_SHORTHAND_GRID,              ///< `grid`: a `grid-template`, or the rows with `auto-flow` columns, or `auto-flow` rows with the columns.
  GTEXT_CSS_SHORTHAND_OFFSET,            ///< `offset`: the position, then the path with its distance and rotation, then `/` and the anchor.
  GTEXT_CSS_SHORTHAND_HOOK,              ///< A shorthand a caller registered with a hook (css_registry.h): the hook splits it.
  GTEXT_CSS_SHORTHAND_KIND_COUNT         ///< The number of kinds; not one.
} GTEXT_CSS_Shorthand_Kind;

/** @brief The name of a kind ("box", "pair" ...). Static; "unknown" for a value that is not one. */
GTEXT_API const char * gtext_css_shorthand_kind_name(GTEXT_CSS_Shorthand_Kind kind);

/**
 * @brief How a shorthand is split.
 *
 * @param registry The registry. Not NULL.
 * @param property A property. An alias is followed.
 * @return The kind; GTEXT_CSS_SHORTHAND_UNIMPLEMENTED for a shorthand with no rule and for a property that is not a shorthand.
 */
GTEXT_API GTEXT_CSS_Shorthand_Kind gtext_css_shorthand_kind(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property);

/** @brief What a shorthand call reports. Plain data; filled whatever happens. */
typedef struct {
  GTEXT_CSS_Status status;      ///< What the call returned.
  const char * message;         ///< A static description; never NULL after a call.
  char name[96];                ///< For GTEXT_CSS_E_UNSUPPORTED: the shorthand (or the property) concerned, cut to fit; else empty.
  const char * detail;          ///< A static sentence saying why, or NULL.
  size_t work;                  ///< Units of work an expansion did (components looked at, longhand texts made, assignments tried), for the quadratic suite.
} GTEXT_CSS_Shorthand_Result;

/** @brief One property a shorthand sets. */
typedef struct {
  GTEXT_CSS_Property_Id property;      ///< The longhand (or reset property).
  GTEXT_CSS_Property_Outcome outcome;  ///< GTEXT_CSS_PROP_RESULT_VALID, _WIDE or _PENDING.
  GTEXT_CSS_Wide_Keyword wide;         ///< WIDE: which keyword.
  const GTEXT_CSS_Value_Set * set;     ///< VALID, PENDING: the value (owned by the list); NULL for WIDE.
  GTEXT_CSS_Property_Id from;          ///< The shorthand it came from.
  bool reset;                          ///< Set only because the shorthand resets it (not one of its longhands).
} GTEXT_CSS_Longhand;

/** @brief The longhands a shorthand expanded to (the struct is private). */
typedef struct GTEXT_CSS_Longhand_List GTEXT_CSS_Longhand_List;

/**
 * @brief Expand a shorthand's value into the values of its longhands.
 *
 * @param registry The registry. Not NULL.
 * @param value What gtext_css_parse_property() returned for a shorthand (VALID, WIDE or PENDING); not INVALID or UNKNOWN. Not NULL.
 * @param allocator NULL for the registry's.
 * @param out Receives the list; written only on success. Free it with gtext_css_longhand_list_free(). Not NULL.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, a property that is not a shorthand, a result that is not VALID, WIDE or PENDING);
 *   GTEXT_CSS_E_UNSUPPORTED (the shorthand has no rule, or a rule whose value it cannot split: the result names it); GTEXT_CSS_E_OOM;
 *   GTEXT_CSS_E_LIMIT_WORK and the limits of the match (a longhand's value could not be decided).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_shorthand_expand(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Property_Value * value,
    const GTEXT_Allocator * allocator, GTEXT_CSS_Longhand_List ** out, GTEXT_CSS_Shorthand_Result * result);

/** @brief How many properties the list sets. NULL is 0. */
GTEXT_API size_t gtext_css_longhand_list_count(const GTEXT_CSS_Longhand_List * list);

/** @brief The `index`th property the list sets; false past the end. The value is owned by the list. */
GTEXT_API bool gtext_css_longhand_list_at(const GTEXT_CSS_Longhand_List * list, size_t index, GTEXT_CSS_Longhand * out);

/** @brief Free a list and the values it owns (a value retained from it, gtext_css_value_set_retain(), outlives it); NULL is ignored. */
GTEXT_API void gtext_css_longhand_list_free(GTEXT_CSS_Longhand_List * list);

/* ---- declaration blocks ------------------------------------------------------------------------- */

/** @brief An ordered list of declarations (the struct is private). */
typedef struct GTEXT_CSS_Declaration_Block GTEXT_CSS_Declaration_Block;

/** @brief The allocator and limits of a block. A zeroed struct is the default. */
typedef struct {
  const GTEXT_Allocator * allocator;  ///< NULL for the registry's.
  size_t max_declarations;            ///< Most declarations (a shorthand counts its longhands). Default 1,048,576; at most 2^28. One more is GTEXT_CSS_E_LIMIT_DECLARATIONS.
  size_t max_output_bytes;            ///< Most bytes serialization writes. Default 64 MiB; 0 selects it. One more is GTEXT_CSS_E_LIMIT_OUTPUT.
} GTEXT_CSS_Declaration_Options;

/** @brief The defaults, spelled out. */
GTEXT_API GTEXT_CSS_Declaration_Options gtext_css_declaration_options_default(void);

/** @brief One declaration of a block. The pointers are owned by the block and valid until it is changed or freed. */
typedef struct {
  GTEXT_CSS_Property_Id property;      ///< The property (a longhand, `all` or a custom property's `--*`; never a shorthand other than `all`).
  GTEXT_CSS_String custom_name;        ///< For a custom property: its name as written, case kept; else empty.
  bool important;                      ///< Declared `!important`.
  GTEXT_CSS_Property_Outcome outcome;  ///< VALID, WIDE or PENDING.
  GTEXT_CSS_Wide_Keyword wide;         ///< WIDE: which keyword.
  const GTEXT_CSS_Value_Set * set;     ///< VALID, PENDING: the value; NULL for WIDE.
  GTEXT_CSS_Property_Id shorthand;     ///< The shorthand it was declared through, or 0.
  size_t declaration;                  ///< The index of the source declaration, equal for the longhands of one shorthand.
} GTEXT_CSS_Declaration_Item;

/**
 * @brief Make an empty block.
 *
 * @param registry The registry. Not NULL; it must outlive the block.
 * @param options NULL for the defaults.
 * @param out Receives it; written only on success. Not NULL.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_block_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Declaration_Options * options,
    GTEXT_CSS_Declaration_Block ** out);

/** @brief Free a block and release its values; NULL is ignored. */
GTEXT_API void gtext_css_block_free(GTEXT_CSS_Declaration_Block * block);

/**
 * @brief Append a declaration.
 *
 * A longhand adds one item; a shorthand its expansion (all-or-nothing: on a failure the block is as it was); a CSS-wide keyword or a pending value
 * adds an item of that kind. `revert` and `revert-layer` are kept as written (the cascade resolves them; the computed-value layer refuses them).
 * The value's set is retained.
 *
 * @param block The block. Not NULL.
 * @param value What gtext_css_parse_property() returned (VALID, WIDE or PENDING). Not NULL.
 * @param important `!important`.
 * @param result NULL, or filled.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, or a result that is INVALID or UNKNOWN); GTEXT_CSS_E_UNSUPPORTED (a shorthand with no
 *   rule: the result names it); GTEXT_CSS_E_LIMIT_DECLARATIONS; GTEXT_CSS_E_OOM; and the statuses of expansion.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_block_add(GTEXT_CSS_Declaration_Block * block, const GTEXT_CSS_Property_Value * value, bool important,
    GTEXT_CSS_Shorthand_Result * result);

/**
 * @brief Append a custom property's declaration.
 *
 * @param block The block. Not NULL.
 * @param name The name as written, beginning `--`; case is kept.
 * @param name_length Its length.
 * @param value What gtext_css_parse_property() returned for it (VALID or WIDE). Not NULL.
 * @param important `!important`.
 * @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID (not a custom property name, or a result that is not VALID or WIDE), GTEXT_CSS_E_LIMIT_DECLARATIONS or GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_block_add_custom(GTEXT_CSS_Declaration_Block * block, const char * name, size_t name_length,
    const GTEXT_CSS_Property_Value * value, bool important);

/**
 * @brief Parse a declaration and, when it is valid, append it.
 *
 * @param block The block. Not NULL.
 * @param name The property's name as written.
 * @param name_length Its length.
 * @param text The value; `!important` is not recognized here (use `important`).
 * @param text_length Its length.
 * @param important `!important`.
 * @param options NULL for the defaults (css_property.h).
 * @param outcome Receives what the parse came to (UNKNOWN and INVALID add nothing); NULL if not wanted.
 * @param result NULL, or filled.
 * @return As gtext_css_block_add(), and the statuses of gtext_css_parse_property_text().
 */
GTEXT_API GTEXT_CSS_Status gtext_css_block_add_text(GTEXT_CSS_Declaration_Block * block, const char * name, size_t name_length,
    const char * text, size_t text_length, bool important, const GTEXT_CSS_Property_Options * options,
    GTEXT_CSS_Property_Outcome * outcome, GTEXT_CSS_Shorthand_Result * result);

/** @brief How many declarations the block holds. NULL is 0. */
GTEXT_API size_t gtext_css_block_count(const GTEXT_CSS_Declaration_Block * block);

/** @brief The `index`th declaration, in the order they were added; false past the end. */
GTEXT_API bool gtext_css_block_item(const GTEXT_CSS_Declaration_Block * block, size_t index, GTEXT_CSS_Declaration_Item * out);

/**
 * @brief The value of a shorthand as the block's longhands serialize it (CSSOM `getPropertyValue(shorthand)`).
 *
 * All the shorthand's longhands (and the properties it resets) must be in the block with equal importance and none pending, else the answer is
 * the empty string; the last declaration of each property counts. A shorthand whose longhands cannot be written as one value (they are not
 * a value of its grammar: `margin-top` auto and the others differ is fine, `font-style: italic` with `font-variant-caps: all-small-caps` is not)
 * is the empty string.
 *
 * @param block The block. Not NULL.
 * @param shorthand A shorthand (an alias is followed).
 * @param out Receives the text in a new buffer, NUL-terminated (release with gtext_css_serialize_buffer_free()); an empty string when there is none.
 * @param out_length Receives its length. Not NULL.
 * @param important Receives the importance of the longhands (NULL if not wanted).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument or a property that is not a shorthand); GTEXT_CSS_E_UNSUPPORTED (a shorthand with
 *   no rule for writing it); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_OUTPUT.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_block_shorthand_value(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id shorthand,
    char ** out, size_t * out_length, bool * important);

/**
 * @brief The value of a property in the block, as CSSOM's `getPropertyValue()` answers: a longhand's last declaration written as a specified value
 *   (in its shortest form), a shorthand's by gtext_css_block_shorthand_value(); the empty string where the block has none.
 *
 * @param block The block. Not NULL.
 * @param property A property (an alias is followed; a custom property cannot be named by its id: use the empty string).
 * @param out Receives the text in a new buffer, NUL-terminated (release with gtext_css_serialize_buffer_free()).
 * @param out_length Receives its length. Not NULL.
 * @param important Receives the importance of the declaration (NULL if not wanted).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_UNSUPPORTED (a shorthand with no rule for writing it); GTEXT_CSS_E_OOM; GTEXT_CSS_E_LIMIT_OUTPUT.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_block_get_property_value(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property,
    char ** out, size_t * out_length, bool * important);

/**
 * @brief Write the block as CSSOM's `cssText`: `name: value;` declarations separated by a space, each shorthand as one declaration when its
 *   longhands allow it, `!important` after the value (preceded by a space).
 *
 * @param block The block. Not NULL.
 * @param sink Where the text goes. Not NULL, with a `write`.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_WRITE; GTEXT_CSS_E_LIMIT_OUTPUT.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_block_serialize(const GTEXT_CSS_Declaration_Block * block, const GTEXT_Markup_Sink * sink);

/** @brief gtext_css_block_serialize() to a new buffer (NUL-terminated; release with gtext_css_serialize_buffer_free()). */
GTEXT_API GTEXT_CSS_Status gtext_css_block_serialize_buffer(const GTEXT_CSS_Declaration_Block * block, char ** out, size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_SHORTHAND_H
