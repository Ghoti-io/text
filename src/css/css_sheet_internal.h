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
 * What a compiled style sheet is made of, shared by the compiler (css_sheet.c) and the rule index (css_rule_index.c). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_SHEET_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_SHEET_INTERNAL_H

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_sheet.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"
#include "css_match_internal.h"
#include "css_selector_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// One valid declaration of a rule, parsed once.
typedef struct {
  GTEXT_CSS_Property_Value pv; ///< The typed value (shared by every element the declaration applies to).
  const char * name;           ///< The name as written, NUL-terminated (arena).
  size_t name_len;
  GTEXT_CSS_Id id;
  GTEXT_CSS_Span span;
  bool important;
  bool custom;
} sheet_decl;

/// The declarations of one rule, in a place among the rules: the order of appearance.
typedef struct {
  sheet_decl * decls;
  size_t ndecl;
  GTEXT_CSS_Id rule_id;
  GTEXT_CSS_Span span;
} sheet_run;

/// One `@scope` rule.
typedef struct sheet_scope {
  const GTEXT_CSS_Selector_List * start; ///< NULL: the implicit scoping root of the registration.
  const GTEXT_CSS_Selector_List * end;   ///< NULL: no scoping limit. Written with the implied `&` and a descendant combinator.
  const css_nest * outer_nest;           ///< What `&` stands for in `start`: the enclosing rule's selector.
  const struct sheet_scope * parent;     ///< The enclosing `@scope`, or NULL.
  size_t depth;                          ///< 1 for the outermost.
} sheet_scope;

/// The kind of a condition node of a sheet.
typedef enum {
  SCK_MEDIA = 1,     ///< `@media`: `list` is the media query list.
  SCK_SUPPORTS,      ///< `@supports`: `list` is the condition.
  SCK_CONTAINER,     ///< `@container`: `list` is the container query list. Element-dependent.
  SCK_STARTING_STYLE, ///< `@starting-style`: always true; the records under it are the starting style's.
  SCK_WHEN,          ///< `@when` or `@else`: `list` is the boolean condition (none: a bare `@else`, always true), `prev` the rule before it in its chain.
  SCK_NEVER,         ///< A rule whose condition is not valid, or `@document`: it never applies.
  SCK_IMPORT         ///< An `@import` with conditions: `list2` is its `supports()` condition, `list` its media query list (each may be NULL).
} sheet_cond_kind;

#define SCF_ELSE 0x1u      ///< `@else`: it applies only if no earlier rule of its chain does.
#define SCF_STARTING 0x2u  ///< The node is `@starting-style` or inside one.
#define SCF_ELEMENT 0x4u   ///< The node is a container query or inside one.

/// A condition node: immutable once the sheet is compiled, shared by every record under it. The ids are from 1, in the order the rules are met (a parent before its children).
typedef struct {
  uint8_t kind;
  uint8_t flags;
  uint32_t parent;                 ///< The enclosing node (0: none).
  uint32_t prev;                   ///< SCK_WHEN: the previous node of the chain (0: it begins one).
  GTEXT_CSS_Media_List * list;
  GTEXT_CSS_Media_List * list2;
  GTEXT_CSS_Span span;
} sheet_cond;

/// Record flags.
enum {
  REC_NESTED = 1u,   ///< Nested in a style rule.
  REC_PSEUDO = 2u,   ///< The selector asks for a pseudo-element: it applies to that, and not to the element.
  REC_HOST = 4u,     ///< The subject compound has `:host`, `:host()` or `:host-context()`: it applies to the host of the shadow tree of the sheet.
  REC_SLOTTED = 8u,  ///< The selector begins its pseudo-element chain with `::slotted()`.
  REC_PART = 16u     ///< ... with `::part()`.
};

/// A complex selector of a rule.
typedef struct {
  const GTEXT_CSS_Selector_List * list;
  uint32_t cx;
  GTEXT_CSS_Specificity spec;
  uint32_t run;
  uint32_t layer;
  const css_nest * nest;
  const sheet_scope * scope;
  uint32_t flags;
  uint32_t cond;   ///< The innermost condition node the rule is under (0: none).
} sheet_record;

/// What the index does with a sheet, in order.
typedef struct {
  uint8_t kind;     ///< 0: declare a layer of the sheet's own tree; 1: place an imported sheet.
  uint32_t node;    ///< 0: the layer node to declare; 1: the layer the import is in (0: none).
  uint32_t import;  ///< 1: which import.
  uint32_t cond;    ///< The condition node the occurrence is under (0: none): the layer is declared, the sheet placed, only where its chain holds.
} sheet_event;

struct GTEXT_CSS_Sheet {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  css_arena arena;
  const GTEXT_CSS_Registry * registry;
  sheet_run * runs;
  size_t nruns, capruns;
  sheet_record * recs;
  size_t nrecs, caprecs;
  const GTEXT_CSS_Selector_List ** lists; ///< Every list the records use (owned).
  size_t nlists, caplists;
  GTEXT_CSS_Layer_Registry * layers; ///< The sheet's own layer tree (author origin).
  uint32_t first_id;                 ///< The registry's id of its first layer: the sheet's layer numbers are dense from 1, `id - first_id + 1`.
  sheet_event * events;
  size_t nevents, capevents;
  GTEXT_CSS_Sheet ** imports;
  size_t nimports, capimports, capimport_aux;
  uint32_t * import_cond;               ///< By import: its `SCK_IMPORT` node (0: none).
  const sheet_scope ** import_scope;    ///< By import: the `@scope` its `scope()` wraps its records in (NULL: none).
  sheet_cond * conds;                   ///< The condition nodes, id - 1.
  size_t nconds, capconds;
  size_t import_depth;
  GTEXT_CSS_Namespace * ns;
  size_t nns;
  const char * default_ns;
  GTEXT_CSS_Sheet_Diagnostic * diags;
  size_t ndiags, capdiags, diag_total;
  size_t bytes;
  bool quirks;
};

/// The layer registry's id of a layer number of the sheet (0 stays 0).
static inline uint32_t sheet_layer_id(const struct GTEXT_CSS_Sheet * s, uint32_t n) {
  return n ? s->first_id + n - 1 : 0;
}

/// The number of the sheet's layer for a registry id.
static inline uint32_t sheet_layer_no(const struct GTEXT_CSS_Sheet * s, uint32_t id) {
  return id ? id - s->first_id + 1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_SHEET_INTERNAL_H
