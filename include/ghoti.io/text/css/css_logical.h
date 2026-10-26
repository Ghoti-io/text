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
 * @file css_logical.h
 * @brief Logical properties: the map from a flow-relative property to the physical one it shares a value with.
 *
 * Reference: CSS Logical Properties and Values Level 1 (Editor's Draft, served 2026-10-26), section 4 ("the mapping depends on the used
 * values of writing-mode, direction, and text-orientation"), and CSS Writing Modes Level 4, section 6.4 (the abstract-to-physical table).
 *
 * ## Groups and slots
 *
 * The pin puts 128 properties in 20 logical property groups (`margin`, `border-width`, `inset`, `size`, `overflow`, `border-radius` ...;
 * GTEXT_CSS_Property_Info::logical_group). A group has a fixed number of **slots** in physical terms: the four sides (top, right, bottom,
 * left) for `margin`, `padding`, `inset`, the border colors, styles, widths and clips and the scroll and overflow-clip margins; the four
 * corners (top-left, top-right, bottom-right, bottom-left) for `border-radius` and `corner-shape`; the two axes (horizontal, vertical) for
 * `size`, `min-size`, `max-size`, `contain-intrinsic-size`, `overflow`, `overscroll-behavior`, `background-position` and `background-repeat`.
 * Each member of a group is either physical (it names a slot: `margin-left`, `width`, `overflow-x`) or flow-relative (`margin-inline-start`,
 * `inline-size`, `border-start-end-radius`, `overflow-block`), and a flow-relative member maps to a slot by the element's flow.
 *
 * "Although the specified value of each property remains distinct, paired properties share a computed value. This shared value is determined
 * by cascading the declarations of both properties together as one." So the map is a function of the **flow** (the computed `writing-mode`,
 * `direction` and `text-orientation` of the element itself) and the cascade of C2b calls it per element; the computed-value layer (css_computed.h)
 * calls it to decide which declaration of a group wins each slot.
 *
 * ## The flow
 *
 * The used direction is the computed `direction`, except that in a vertical writing mode (`vertical-rl`, `vertical-lr`) `text-orientation: upright`
 * forces `ltr` (Writing Modes 4: "The used direction depends on the computed writing-mode and text-orientation"). `sideways-rl` and
 * `sideways-lr` typeset horizontally, so `text-orientation` does not change them. The table of Writing Modes 4, 6.4, flattened:
 *
 * | writing-mode | block-start | block-end | inline-start (ltr, rtl) | inline-end (ltr, rtl) |
 * |---|---|---|---|---|
 * | horizontal-tb | top | bottom | left, right | right, left |
 * | vertical-rl, sideways-rl | right | left | top, bottom | bottom, top |
 * | vertical-lr | left | right | top, bottom | bottom, top |
 * | sideways-lr | left | right | bottom, top | top, bottom |
 *
 * `block-size` is the height in a horizontal mode and the width otherwise, `inline-size` the other. A corner is named by its block side and
 * its inline side (`border-start-end-radius` is the block-start, inline-end corner). The functions here are pure and thread-safe.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_LOGICAL_H
#define GHOTI_IO_GTEXT_CSS_CSS_LOGICAL_H

#include <ghoti.io/text/css/css_element.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The values of `writing-mode`. */
typedef enum {
  GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB = 0,
  GTEXT_CSS_WRITING_MODE_VERTICAL_RL,
  GTEXT_CSS_WRITING_MODE_VERTICAL_LR,
  GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL,
  GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR
} GTEXT_CSS_Writing_Mode;

/** @brief The values of `text-orientation`. */
typedef enum {
  GTEXT_CSS_TEXT_ORIENTATION_MIXED = 0,
  GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT,
  GTEXT_CSS_TEXT_ORIENTATION_SIDEWAYS
} GTEXT_CSS_Text_Orientation;

/** @brief The three computed values the map depends on. A zeroed struct is `horizontal-tb`, `ltr`, `mixed`. */
typedef struct {
  GTEXT_CSS_Writing_Mode writing_mode;     ///< The computed `writing-mode`.
  GTEXT_CSS_Direction direction;           ///< The computed `direction`.
  GTEXT_CSS_Text_Orientation text_orientation; ///< The computed `text-orientation`.
} GTEXT_CSS_Flow;

/** @brief What kind of slots a logical group has. */
typedef enum {
  GTEXT_CSS_LOGICAL_NONE = 0, ///< Not a logical group.
  GTEXT_CSS_LOGICAL_SIDES,    ///< Four slots: top, right, bottom, left (slot 0 to 3).
  GTEXT_CSS_LOGICAL_CORNERS,  ///< Four slots: top-left, top-right, bottom-right, bottom-left (slot 0 to 3).
  GTEXT_CSS_LOGICAL_AXES      ///< Two slots: horizontal (`width`, `x`), vertical (`height`, `y`) (slot 0 and 1).
} GTEXT_CSS_Logical_Kind;

/** @brief The sides, as the slots of GTEXT_CSS_LOGICAL_SIDES. */
typedef enum { GTEXT_CSS_SIDE_TOP = 0, GTEXT_CSS_SIDE_RIGHT, GTEXT_CSS_SIDE_BOTTOM, GTEXT_CSS_SIDE_LEFT } GTEXT_CSS_Side;

/** @brief The corners, as the slots of GTEXT_CSS_LOGICAL_CORNERS. */
typedef enum {
  GTEXT_CSS_CORNER_TOP_LEFT = 0,
  GTEXT_CSS_CORNER_TOP_RIGHT,
  GTEXT_CSS_CORNER_BOTTOM_RIGHT,
  GTEXT_CSS_CORNER_BOTTOM_LEFT
} GTEXT_CSS_Corner;

/** @brief The axes, as the slots of GTEXT_CSS_LOGICAL_AXES. */
typedef enum { GTEXT_CSS_AXIS_HORIZONTAL = 0, GTEXT_CSS_AXIS_VERTICAL } GTEXT_CSS_Axis;

/** @brief The used direction (see the file's description): `direction`, except `ltr` for a vertical mode with `text-orientation: upright`. */
GTEXT_API GTEXT_CSS_Direction gtext_css_flow_used_direction(const GTEXT_CSS_Flow * flow);

/** @brief The side that block-start (block_end false) or block-end (true) is, by the writing mode. */
GTEXT_API GTEXT_CSS_Side gtext_css_flow_block_side(const GTEXT_CSS_Flow * flow, bool block_end);

/** @brief The side that inline-start (inline_end false) or inline-end (true) is, by the writing mode and the used direction. */
GTEXT_API GTEXT_CSS_Side gtext_css_flow_inline_side(const GTEXT_CSS_Flow * flow, bool inline_end);

/** @brief The axis that the block axis (inline false) or the inline axis (true) is: block is vertical in `horizontal-tb`, horizontal otherwise. */
GTEXT_API GTEXT_CSS_Axis gtext_css_flow_axis(const GTEXT_CSS_Flow * flow, bool inline_axis);

/**
 * @struct GTEXT_CSS_Logical_Member
 * @brief What the name of a member of a logical group says about it.
 */
typedef struct {
  GTEXT_CSS_Logical_Kind kind;  ///< The kind of the group (NONE when the property is in no group).
  const char * group;           ///< The group's name (`margin`), static; NULL when there is none.
  bool logical;                 ///< The member is flow-relative; false for a physical one (which names its slot).
  size_t slot;                  ///< A physical member: its slot. A logical one: not meaningful (see gtext_css_logical_slot()).
} GTEXT_CSS_Logical_Member;

/**
 * @brief What a property is in its logical group.
 *
 * Reads the property's registry entry and its name: a name that carries `block-start`, `block-end`, `inline-start`, `inline-end`,
 * `start-start`, `start-end`, `end-start`, `end-end`, `block` or `inline` (as a hyphen-delimited word) is flow-relative, one that carries
 * `top`, `right`, `bottom`, `left`, `top-left` ..., `width`, `height`, `x`, `y` is physical.
 *
 * @param registry The registry. Not NULL.
 * @param property The property (an alias is followed).
 * @param out Receives it; written whatever happens (`kind` NONE for a property that is in no group). Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument or a property the registry lacks); GTEXT_CSS_E_UNSUPPORTED (the property's group
 *   is not one the name rules read: a caller's own `logical_group`).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_logical_member(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property,
    GTEXT_CSS_Logical_Member * out);

/**
 * @brief The slot of the group that a member is, in this flow.
 *
 * A physical member is its own slot. A flow-relative one is mapped by the flow: `margin-inline-start` is slot LEFT in `horizontal-tb`
 * `ltr`, RIGHT in `rtl`, TOP in `vertical-rl`.
 *
 * @param registry The registry. Not NULL.
 * @param property A member of a group.
 * @param flow The flow. Not NULL.
 * @param kind Receives the group's kind (NULL if not wanted).
 * @param slot Receives the slot. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (the property is in no group, or a NULL argument).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_logical_slot(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Flow * flow, GTEXT_CSS_Logical_Kind * kind, size_t * slot);

/**
 * @brief The physical property that a member shares its value with: the physical member of its group that is the same slot.
 *
 * A physical member maps to itself. A caller's override of a physical member (css_registry.h) is found in its place.
 *
 * @param registry The registry. Not NULL.
 * @param property A member of a group.
 * @param flow The flow. Not NULL.
 * @param physical Receives the physical property. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (no group, or a NULL argument); GTEXT_CSS_E_UNSUPPORTED (the group has no physical member for the slot).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_logical_map(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Flow * flow, GTEXT_CSS_Property_Id * physical);

/**
 * @brief How many members of the group map to the slot of `property` in this flow, and the `index`th of them.
 *
 * This is the inverse of gtext_css_logical_slot(): for `margin-left` in `horizontal-tb` `ltr` the members are `margin-left` and
 * `margin-inline-start`. They are in the order of the group (the sorted names of the pin).
 *
 * @param registry The registry. Not NULL.
 * @param property A member of a group.
 * @param flow The flow. Not NULL.
 * @param index Which one, below the count.
 * @param count Receives how many (NULL if not wanted).
 * @param member Receives the `index`th (NULL if not wanted); GTEXT_CSS_PROPERTY_NONE for an index past the count.
 * @return GTEXT_CSS_OK, or GTEXT_CSS_E_INVALID.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_logical_sharers(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Flow * flow, size_t index, size_t * count, GTEXT_CSS_Property_Id * member);

/** @brief The name of a writing mode ("horizontal-tb" ...); NULL for a value that is not one. Static. */
GTEXT_API const char * gtext_css_writing_mode_name(GTEXT_CSS_Writing_Mode mode);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_LOGICAL_H
