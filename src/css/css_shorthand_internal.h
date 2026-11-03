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
 * @file
 *
 * What the shorthand files share: the declaration block, the text buffer, the exact writing of a typed value (every number as the shortest
 * digits that read back, not CSSOM's six decimals, because a longhand made from a shorthand's text must keep what was written), and the
 * parse of a longhand's text. Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_SHORTHAND_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_SHORTHAND_INTERNAL_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_shorthand.h>
#include <ghoti.io/text/macros.h>

#include "css_compute_tables.h"
#include "css_internal.h"
#include "css_property_canon.h"
#include "css_registry_internal.h"
#include "css_tables.h"
#include "css_value_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A growable text buffer.
typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
  bool oom;
} tbuf;

void tb_init(tbuf * b, const GTEXT_Allocator * alloc);
bool tb_put(tbuf * b, const char * s, size_t n);
bool tb_puts(tbuf * b, const char * s);
/// The text, NUL-terminated and owned by the caller afterward; the buffer is empty again.
char * tb_take(tbuf * b, size_t * len);
void tb_free(tbuf * b);

/// Write a typed value as CSS text, exactly: numbers are the shortest digits that read back. False on memory.
bool css_exact_text(tbuf * b, const GTEXT_CSS_Value * v);

/// A number written exactly (at least 64 bytes of `out`); returns the length.
size_t css_exact_number(double v, char * out);

/// The class row of a property in the generated table (css_compute.c), following an override to the property of the same name in the pin.
const css_tbl_compute * css_compute_row(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id, GTEXT_CSS_Property_Id * generated_id);

/// The block's item.
typedef struct {
  GTEXT_CSS_Property_Id property;
  char * custom_name;
  size_t custom_length;
  bool important;
  GTEXT_CSS_Property_Outcome outcome;
  GTEXT_CSS_Wide_Keyword wide;
  GTEXT_CSS_Value_Set * set;   ///< Retained.
  GTEXT_CSS_Property_Id shorthand;
  size_t declaration;
  bool opaque;                 ///< A declaration kept as written (gtext_css_block_set_opaque()): `raw` holds the value; `set` is NULL.
  char * raw;                  ///< Opaque: the value text (NUL-terminated).
  size_t raw_length;
  bool dead;                   ///< Removed (gtext_css_block_remove()): a slot that every reader skips until the next compaction. Its set and name are released.
  size_t prev;                 ///< With the index: the slot + 1 of the previous item of the same property or custom name (0: none); the chain is what a removal kills whole.
} blk_item;

struct GTEXT_CSS_Declaration_Block {
  const GTEXT_CSS_Registry * registry;
  GTEXT_Allocator alloc;
  GTEXT_CSS_Declaration_Options options;
  blk_item * items;
  size_t count, cap;           ///< Slots used (live and dead) and allocated.
  size_t live;                 ///< Slots that are not dead: what gtext_css_block_count() answers.
  size_t next_declaration;
  /* The index (css_block.c): open addressing over the property id, or the case-sensitive name of a custom property, to the last item of that key. It is built by the first
     operation that needs it (set, remove, find on a mutable block) and kept from then on, so a block that only adds and is read (the cascade's) pays nothing for it. */
  size_t * index;              ///< NULL, or `index_cap` entries: 0 empty, SIZE_MAX a tombstone, else the slot + 1 of the last item of a key.
  size_t index_cap;            ///< A power of two.
  size_t index_used;           ///< Entries that are not empty (tombstones included).
  size_t index_tombs;          ///< Tombstones.
  uint32_t * covered;          ///< NULL, or one count per property id: the live opaque shorthands whose longhands include it (built by the first opaque shorthand).
  size_t opaque_shorthands;    ///< Live opaque items whose property is a shorthand.
  uint64_t probes;             ///< Entries looked at by the index since the block was made (the counter the cost tests assert).
  bool parse_mode;             ///< The block is being built from a parsed list of declarations (css_decl_block_from_declarations): a later declaration of a property **removes** the earlier one and is appended
                               ///< (both browsers write `color:red;margin:1px;color:blue` as `margin: 1px; color: blue;`), where setProperty() replaces in place. An important one is still kept.
};

/// A mutable block's items as css_block.c keeps them, for the writer of cssText and the shorthand text: slot `i` is dead when `b->items[i].dead`.
/// The properties and custom names a shorthand sets (its longhands and the properties it resets, nested shorthands followed); false when there are more than `cap`.
/// Squeeze out the slots that removals left dead (the order of the rest is kept). Nothing is allocated.
void css_block_compact(GTEXT_CSS_Declaration_Block * block);
/// The block's cssText into a buffer (css_shorthand_serialize.c).
GTEXT_CSS_Status css_block_text(const GTEXT_CSS_Declaration_Block * block, tbuf * out);

/// gtext_css_block_set() and _set_custom() with the rule of a list of declarations: `keep_important` leaves an important declaration of the property alone when the new one is not.
GTEXT_CSS_Status css_block_set_ex(GTEXT_CSS_Declaration_Block * block, const GTEXT_CSS_Property_Value * value, bool important, bool keep_important,
    GTEXT_CSS_Block_Change * change, GTEXT_CSS_Shorthand_Result * result);
GTEXT_CSS_Status css_block_set_custom_ex(GTEXT_CSS_Declaration_Block * block, const char * name, size_t name_length, const GTEXT_CSS_Property_Value * value,
    bool important, bool keep_important, GTEXT_CSS_Block_Change * change);
/// gtext_css_block_set_opaque() with the rule of a list of declarations: `keep_important` leaves an important declaration of the property alone when the new one is not.
GTEXT_CSS_Status css_block_set_opaque_ex(GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id property, const char * raw, size_t raw_length, bool important,
    bool keep_important, GTEXT_CSS_Block_Change * change);
/// Build the block's index now (the first operation that needs it does anyway): the only step of a find that can fail.
GTEXT_CSS_Status css_block_prepare(GTEXT_CSS_Declaration_Block * block);

/// How two blocks differ, declaration by declaration: the longhands (ascending by id, without repeats) and custom properties (ascending by name) whose last declaration
/// is in one block only or says something else in the other. The custom names point into the blocks' memory.
typedef struct {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Property_Id * ids;
  size_t id_count;
  GTEXT_CSS_String * customs;
  size_t custom_count;
} css_block_delta;
GTEXT_CSS_Status css_block_diff(GTEXT_CSS_Declaration_Block * from, GTEXT_CSS_Declaration_Block * to, css_block_delta * diff);
void css_block_diff_free(css_block_delta * diff);

#define CSS_LEAF_CAP 1024u   ///< Room for the leaves of any shorthand: `all` has about 600.

bool css_shorthand_leaves(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id shorthand, GTEXT_CSS_Property_Id * out, size_t cap, size_t * n);

/// Parse text as the value of a property. A VALID, WIDE or PENDING outcome fills `pv`; any other (INVALID, UNKNOWN) is OK with that outcome.
GTEXT_CSS_Status css_parse_longhand(const GTEXT_CSS_Registry * registry, const GTEXT_Allocator * alloc, GTEXT_CSS_Property_Id property,
    const char * text, size_t length, GTEXT_CSS_Property_Value * pv);

/// The serialization of a property's value as CSSOM writes a specified value (the shortest form where the property has a rule for it): a new string.
GTEXT_CSS_Status css_item_text(const GTEXT_CSS_Registry * registry, const GTEXT_Allocator * alloc, GTEXT_CSS_Property_Id property,
    GTEXT_CSS_Property_Outcome outcome, GTEXT_CSS_Wide_Keyword wide, const GTEXT_CSS_Value_Set * set, char ** out, size_t * out_len);

/// The initial value text of a property (a shorthand's is NULL).
const char * css_initial_text(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property);

/// The shorthand text a block's longhands make (css_shorthand_serialize.c): `*out` is a new string, empty when the longhands do not make one.
GTEXT_CSS_Status css_block_shorthand_text(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id shorthand, bool * all_wide,
    GTEXT_CSS_Wide_Keyword * wide, char ** out, size_t * out_len, bool * important, GTEXT_CSS_Shorthand_Result * result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_SHORTHAND_INTERNAL_H
