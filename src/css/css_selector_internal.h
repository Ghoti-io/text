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
 * What the selector files share (css_selector_parse.c, _serialize.c, _specificity.c, and the matcher and the style sheet that read the compiled form). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_SELECTOR_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_SELECTOR_INTERNAL_H

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/css/css_selector.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"
#include "css_selector_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The private part of a list: its memory and the summaries computed once at the end of the parse.
struct GTEXT_CSS_Selector_List_Internal {
  GCU_Atomic_Int refs;           ///< Owners.
  GTEXT_Allocator alloc;         ///< What the arena's blocks come from.
  css_arena arena;               ///< Every node, string and array of the list and the lists inside it.
  GTEXT_CSS_Selector_Requires req; ///< gtext_css_selector_requires().
  GTEXT_CSS_Selector_Dependencies dep; ///< gtext_css_selector_dependencies().
};

/// The row of a pseudo-class or pseudo-element simple selector.
static inline const sel_row * css_sel_row_of(const GTEXT_CSS_Simple * s) {
  return &sel_rows[s->pseudo];
}

/// ASCII lower-case of one byte.
static inline unsigned char css_sel_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

/// ASCII case-insensitive equality of two byte strings.
static inline bool css_sel_ieq(const char * a, size_t an, const char * b, size_t bn) {
  if (an != bn) {
    return false;
  }
  for (size_t i = 0; i < an; i++) {
    if (css_sel_lower((unsigned char)a[i]) != css_sel_lower((unsigned char)b[i])) {
      return false;
    }
  }
  return true;
}

/// Fill the summaries of a finished list (the dependencies and what it requires), walking it and the lists inside it.
bool css_selector_analyze(struct GTEXT_CSS_Selector_List_Internal * in, GTEXT_CSS_Selector_List * list);

/// The specificity of one simple selector (with `nesting` for `&`), and of a compound and of a complex selector.
GTEXT_CSS_Specificity css_selector_simple_specificity(const GTEXT_CSS_Simple * s, GTEXT_CSS_Specificity nesting);

/// Whether a pseudo-class row is one of the rows the matcher refers to.
#define CSS_SEL_IS(s, row) ((s)->kind == GTEXT_CSS_SIMPLE_PSEUDO_CLASS && (s)->pseudo == (row))

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_SELECTOR_INTERNAL_H
