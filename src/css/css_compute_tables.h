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
 * The layout of the generated computed-value tables (src/css/tables/css_tables_compute.c, written by tools/css/gen-compute-tables.py). Private.
 */

#ifndef GHOTI_IO_GTEXT_CSS_COMPUTE_TABLES_H
#define GHOTI_IO_GTEXT_CSS_COMPUTE_TABLES_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/css/css_computed.h>
#include <ghoti.io/text/css/css_shorthand.h>
#include <stddef.h>
#include <stdint.h>

/** One row for each generated property, in the order of its id (the sorted names of the pin). */
typedef struct {
  uint8_t klass;          ///< GTEXT_CSS_Compute_Class.
  uint8_t algo;           ///< GTEXT_CSS_Compute_Algorithm.
  uint8_t shorthand;      ///< GTEXT_CSS_Shorthand_Kind.
  uint8_t flags;          ///< CSS_TBL_COMPUTE_*.
  uint16_t order;         ///< Position in the order of computation, from 1; 0 for none.
  uint16_t initial_len;   ///< Length of the initial value override in the pool; 0 for none.
  uint32_t initial_off;   ///< Its offset in gtext_css_tbl_compute_pool.
} css_tbl_compute;

#define CSS_TBL_COMPUTE_REQUIRED 0x01u ///< Inherited, or an SVG 2 presentation attribute: never unimplemented.
#define CSS_TBL_COMPUTE_PARTIAL 0x02u  ///< The algorithm leaves something of the specification undone (the row of computed_rules.tsv says what).

extern const uint64_t gtext_css_tbl_compute_names_hash;
extern const size_t gtext_css_tbl_compute_count;
extern const css_tbl_compute gtext_css_tbl_compute[];
extern const char gtext_css_tbl_compute_pool[];
extern const size_t gtext_css_tbl_compute_pool_size;
extern const size_t gtext_css_tbl_compute_order_count;
extern const uint32_t gtext_css_tbl_compute_order[];

#endif /* GHOTI_IO_GTEXT_CSS_COMPUTE_TABLES_H */
