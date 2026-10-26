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
 * The layout of the generated registry tables (src/css/tables/, written by tools/css/gen-css-tables.py). Private.
 *
 * Every table is const data that holds no pointer, so that the shared library carries no relocation for it: a
 * string is an offset and a length into one pool (css_tables_pool.c, every string NUL-terminated, so that an
 * offset is also a C string), and a reference to another row is that row's index plus one (0 is none). The tables
 * of names are sorted by the bytes of the name, which is a contract with the lookups in css_registry.c.
 */

#ifndef GHOTI_IO_GTEXT_CSS_TABLES_H
#define GHOTI_IO_GTEXT_CSS_TABLES_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/css/css_registry.h>
#include <stddef.h>
#include <stdint.h>

/** A string of the pool: where it starts and its length (not counting the NUL). {0, 0} is the empty string. */
typedef struct {
  uint32_t off;
  uint32_t len;
} css_tstr;

typedef struct {
  css_tstr name, grammar, initial, inherited_raw, animation_raw, computed_value, applies_to, percentages,
      canonical_order, source;
  uint32_t group;                        ///< Logical group index + 1, or 0.
  uint32_t alias_of;                     ///< Property index + 1, or 0.
  uint32_t longhand_first, longhand_count; ///< Into gtext_css_tbl_property_refs.
  uint32_t reset_first, reset_count;     ///< Likewise.
  uint32_t style_first, style_count;     ///< Into gtext_css_tbl_style_names.
  uint8_t inherited;                     ///< GTEXT_CSS_Inherit (derived for a shorthand).
  uint8_t animation;                     ///< GTEXT_CSS_Animation.
  uint8_t flags;                         ///< GTEXT_CSS_PROP_* (the generated bits).
} css_tbl_property;

typedef struct {
  css_tstr name;
  uint32_t first, count;                 ///< Members: into gtext_css_tbl_group_members.
} css_tbl_group;

typedef struct {
  css_tstr name, grammar, source;
  uint32_t scope;                        ///< Property index + 1 of the property that defines it, or 0.
  uint8_t flags;                         ///< GTEXT_CSS_TYPE_*.
} css_tbl_type;

typedef struct {
  css_tstr name, grammar, source;
  uint32_t first, count;                 ///< Its descriptors: rows of gtext_css_tbl_descriptors.
  uint8_t declarations;
} css_tbl_at_rule;

typedef struct {
  css_tstr name, grammar, initial;
  uint32_t at_rule;                      ///< At-rule index + 1.
  uint8_t kind;                          ///< GTEXT_CSS_Descriptor_Kind.
} css_tbl_descriptor;

typedef struct {
  css_tstr name, syntax, source;
} css_tbl_selector;

typedef struct {
  css_tstr name, measures;
} css_tbl_unit;

extern const char gtext_css_tbl_pool[];
extern const size_t gtext_css_tbl_pool_size;

extern const size_t gtext_css_tbl_property_count;
extern const css_tbl_property gtext_css_tbl_properties[];
extern const uint32_t gtext_css_tbl_property_refs[];
extern const size_t gtext_css_tbl_property_ref_count;
extern const css_tstr gtext_css_tbl_style_names[];
extern const size_t gtext_css_tbl_group_count;
extern const css_tbl_group gtext_css_tbl_groups[];
extern const uint32_t gtext_css_tbl_group_members[];

extern const size_t gtext_css_tbl_type_count;
extern const css_tbl_type gtext_css_tbl_types[];

extern const size_t gtext_css_tbl_at_rule_count;
extern const css_tbl_at_rule gtext_css_tbl_at_rules[];
extern const size_t gtext_css_tbl_descriptor_count;
extern const css_tbl_descriptor gtext_css_tbl_descriptors[];

extern const size_t gtext_css_tbl_selector_count;
extern const css_tbl_selector gtext_css_tbl_selectors[];
extern const size_t gtext_css_tbl_keyword_count;
extern const css_tstr gtext_css_tbl_keywords[];
extern const size_t gtext_css_tbl_unit_count;
extern const css_tbl_unit gtext_css_tbl_units[];
extern const size_t gtext_css_tbl_function_count;
extern const css_tstr gtext_css_tbl_functions[];
extern const char gtext_css_tbl_pin_commit[];

/** The C string of a pool string, or NULL for the empty one. */
static inline const char * css_tstr_c(css_tstr s) {
  return s.len ? gtext_css_tbl_pool + s.off : NULL;
}

#endif /* GHOTI_IO_GTEXT_CSS_TABLES_H */
