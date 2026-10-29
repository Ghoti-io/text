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
 * What the at-rule files share (css_atrule.c, css_font_rules.c, css_counter_style.c, css_page.c, css_keyframes.c, css_atrule_set.c). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_ATRULE_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_ATRULE_INTERNAL_H

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/css/css_atrule.h>
#include <ghoti.io/text/css/css_page.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// One valid descriptor or declaration of an at-rule.
typedef struct {
  char * name;
  size_t name_len;
  GTEXT_CSS_Descriptor_Id desc;
  GTEXT_CSS_Property_Id prop;
  bool property;
  bool important;
  GTEXT_CSS_Property_Value pv;
  char * text;
  size_t text_len;
  GTEXT_CSS_Id decl;
  GTEXT_CSS_Span span;
} atrule_desc;

struct GTEXT_CSS_Atrule {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  css_arena arena;
  const GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Atrule_Kind kind;
  const char * name;
  size_t name_len;
  GTEXT_CSS_Span span;
  GTEXT_CSS_Id id;
  const char * prelude;
  size_t prelude_len;
  GTEXT_CSS_String * names;
  bool * quoted;
  size_t nnames;
  atrule_desc * ds;
  size_t nds, capds;
  struct GTEXT_CSS_Atrule ** kids;
  size_t nkids, capkids;
  GTEXT_CSS_Atrule_Diagnostic * diags;
  size_t ndiags, capdiags, diag_total;
  bool defines_nothing;
  double * offsets;                  ///< A keyframe block: its offsets (0 to 1), in order, duplicates kept.
  size_t noffsets;
  char ** seltext;                   ///< Per offset: the selector as written when its offset needs a timeline or `sibling-index()` (the offset is then -1), else NULL.
  GTEXT_CSS_Page_Selector * sels;    ///< A page: its selectors.
  size_t nsels;
  size_t max_children;
  size_t max_descriptors;
};

/// The row of the generated table for an at-rule name (lower case, no `@`), or NULL.
const void * css_atrule_row_find(const char * name, size_t length);

/// The kind of a row.
GTEXT_CSS_Atrule_Kind css_atrule_row_kind(const void * row);

/// Read the selectors of a page rule's prelude. *valid false: the prelude is not a `<page-selector-list>`.
GTEXT_CSS_Status css_page_selectors_parse(const GTEXT_Allocator * alloc, css_arena * arena, const GTEXT_CSS_Component_Value * values, size_t count, size_t max,
    GTEXT_CSS_Page_Selector ** out, size_t * n, bool * valid);

/// The find of a descriptor in an atrule by lower-case name (internal; returns the index or -1).
int css_atrule_find_index(const struct GTEXT_CSS_Atrule * a, const char * name, size_t length);

/// The canonical text of a descriptor, read as component values (the typed readers parse the registry's own serialization). *tree is NULL when the rule has no such descriptor. Free it with gtext_css_tree_free().
GTEXT_CSS_Status css_atrule_tokens(const struct GTEXT_CSS_Atrule * a, const char * name, GTEXT_CSS_Tree ** tree);

/// A descriptor's serialized value.
const atrule_desc * css_atrule_desc_at(const struct GTEXT_CSS_Atrule * a, size_t i);

/// The arena a typed view (a font face, a counter style, keyframes) keeps its strings and arrays in.
typedef struct {
  GTEXT_Allocator alloc;
  css_arena arena;
} holder;

static inline holder * holder_new(const GTEXT_Allocator * alloc) {
  holder * h = gtext_allocator_calloc(alloc, 1, sizeof *h);
  if (!h) {
    return NULL;
  }
  h->alloc = *alloc;
  css_arena_init(&h->arena, &h->alloc);
  return h;
}

static inline void holder_free(holder * h) {
  if (h) {
    const GTEXT_Allocator a = h->alloc;
    css_arena_free(&h->arena);
    gtext_allocator_free(&a, h);
  }
}

/// A number token as an integer, clamped to the range of the type (a cast of a huge double is undefined).
static inline uint32_t clamp_u32(double v) {
  return v >= 4294967295.0 ? 0xFFFFFFFFu : (v > 0 ? (uint32_t)v : 0u);
}

static inline int64_t clamp_i64(double v) {
  return v >= 9223372036854775807.0 ? INT64_MAX : (v <= -9223372036854775808.0 ? INT64_MIN : (int64_t)v);
}

/// Token readers the typed views share.
static inline const GTEXT_CSS_Component_Value * next_sig(const GTEXT_CSS_Component_Value * v, size_t n, size_t * i) {
  while (*i < n && v[*i].kind == GTEXT_CSS_VALUE_TOKEN && v[*i].type == GTEXT_CSS_TOKEN_WHITESPACE) {
    (*i)++;
  }
  return *i < n ? &v[(*i)++] : NULL;
}

static inline bool is_comma(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_COMMA;
}

static inline bool is_ident(const GTEXT_CSS_Component_Value * v, const char * lit) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT && css_ascii_ieq(&v->value, lit);
}

static inline GTEXT_CSS_String copy_string(holder * h, const GTEXT_CSS_String * s, bool * oom) {
  GTEXT_CSS_String out = {NULL, 0};
  char * c = css_arena_strdup(&h->arena, s->data ? s->data : "", s->length);
  if (!c) {
    *oom = true;
    return out;
  }
  out.data = c;
  out.length = s->length;
  return out;
}

#ifdef __cplusplus
}
#endif

/// Whether an `@property` rule is valid, judged by the code that registers it (css_at_property.c): a valid name, `syntax`, `inherits` and an `initial-value` that holds for
/// the syntax. *why receives the sentence of the first reason it is not (static), or NULL. A registration of the same name by the caller does not make the rule invalid.
GTEXT_CSS_Status css_at_property_rule_validity(const GTEXT_CSS_Registry * registry, const GTEXT_Allocator * allocator, const GTEXT_CSS_Rule * rule, bool * valid,
    const char ** why);

#endif
