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
} blk_item;

struct GTEXT_CSS_Declaration_Block {
  const GTEXT_CSS_Registry * registry;
  GTEXT_Allocator alloc;
  GTEXT_CSS_Declaration_Options options;
  blk_item * items;
  size_t count, cap;
  size_t next_declaration;
};

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
