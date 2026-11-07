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
 * The kinds of rule (css_cssom_rule.h): what CSSOM does with each (the generated table), the kind of a syntax node, and the walk over what a rule's block holds that becomes
 * rules. The walk is the one definition of which of a block's contents are rules: a parse builds the objects with it, and a compaction pairs the objects with the nodes of a
 * copy of the tree with it, so the two cannot disagree.
 */

#include <string.h>

#include "css_atrule_internal.h"
#include "cssom_kind_tables.h"
#include "cssom_rule_internal.h"

_Static_assert(CSSOM_KIND_ROW_COUNT == GTEXT_CSSOM_RULE_KIND_COUNT, "the kinds table has a row for each kind of rule");
_Static_assert(CSS_KIND_RULE == CSSOM_KIND_RULE && CSS_KIND_LIST_RULES == CSSOM_KIND_LIST_RULES && CSS_KIND_LIST_KEYFRAMES == CSSOM_KIND_LIST_KEYFRAMES &&
                   CSS_KIND_LIST_MARGINS == CSSOM_KIND_LIST_MARGINS && CSS_KIND_STYLE_PROPERTIES == CSSOM_KIND_STYLE_PROPERTIES &&
                   CSS_KIND_STYLE_DESCRIPTORS == CSSOM_KIND_STYLE_DESCRIPTORS && CSS_KIND_INSERTABLE == CSSOM_KIND_INSERTABLE,
    "the flags of cssom_rule_internal.h are the generated table's");
_Static_assert(sizeof(css_kind_info) == sizeof(cssom_kind_row) && offsetof(css_kind_info, flags) == offsetof(cssom_kind_row, flags) &&
                   offsetof(css_kind_info, parents) == offsetof(cssom_kind_row, parents),
    "css_kind_info is the generated row");

const css_kind_info * css_kind(GTEXT_CSSOM_Rule_Kind kind) {
  return (unsigned)kind < (unsigned)GTEXT_CSSOM_RULE_KIND_COUNT ? (const css_kind_info *)&cssom_kind_rows[kind] : (const css_kind_info *)&cssom_kind_rows[GTEXT_CSSOM_RULE_UNKNOWN];
}

const char * gtext_cssom_rule_kind_name(GTEXT_CSSOM_Rule_Kind kind) {
  return (unsigned)kind < (unsigned)GTEXT_CSSOM_RULE_KIND_COUNT ? cssom_kind_rows[kind].name : "unknown";
}

GTEXT_CSSOM_Rule_Kind css_kind_of_node(const GTEXT_CSS_Rule * node, GTEXT_CSSOM_Rule_Kind parent_kind, bool top) {
  switch (node->kind) {
  case GTEXT_CSS_RULE_QUALIFIED:
    return !top && parent_kind == GTEXT_CSSOM_RULE_KEYFRAMES ? GTEXT_CSSOM_RULE_KEYFRAME : GTEXT_CSSOM_RULE_STYLE;
  case GTEXT_CSS_RULE_NESTED_DECLARATIONS:
    return GTEXT_CSSOM_RULE_NESTED_DECLARATIONS;
  default:
    break;
  }
  const void * row = css_atrule_row_find(node->name.data, node->name.length);
  if (row) {
    return (GTEXT_CSSOM_Rule_Kind)css_atrule_row_kind(row);
  }
  return !top && parent_kind == GTEXT_CSSOM_RULE_FONT_FEATURE_VALUES ? GTEXT_CSSOM_RULE_FEATURE_BLOCK : GTEXT_CSSOM_RULE_UNKNOWN;
}

void css_child_walk_init(css_child_walk * walk, GTEXT_CSSOM_Rule_Kind parent_kind, const GTEXT_CSS_Rule * node, bool nested) {
  walk->node = node;
  walk->parent_kind = parent_kind;
  /* the body of a mixin, the contents block an `@apply` passes and the fallback block of an `@contents` are each "treated as a nested declarations rule" (CSS Functions and Mixins 1), so
     the declarations in them are CSSNestedDeclarations rules and the rules in them are read as nested ones */
  walk->nested = nested || parent_kind == GTEXT_CSSOM_RULE_MIXIN || parent_kind == GTEXT_CSSOM_RULE_APPLY || parent_kind == GTEXT_CSSOM_RULE_CONTENTS;
  walk->index = 0;
}

/// Whether a child of the kind found under a parent of this kind is a member of the parent's `cssRules`.
static bool member_of(GTEXT_CSSOM_Rule_Kind parent, GTEXT_CSSOM_Rule_Kind child) {
  const unsigned list = css_kind(parent)->flags;
  if (list & CSS_KIND_LIST_KEYFRAMES) {
    return child == GTEXT_CSSOM_RULE_KEYFRAME;
  }
  if (list & CSS_KIND_LIST_MARGINS) {
    return child == GTEXT_CSSOM_RULE_PAGE_MARGIN;
  }
  if (list & CSS_KIND_LIST_RULES) {
    /* a keyframe and a margin rule are members of their own parents only; a kind that is not a rule is nobody's */
    return (css_kind(child)->flags & CSS_KIND_RULE) && child != GTEXT_CSSOM_RULE_KEYFRAME && child != GTEXT_CSSOM_RULE_PAGE_MARGIN;
  }
  return false;
}

bool css_child_walk_next(css_child_walk * walk, css_child * out) {
  const GTEXT_CSS_Rule * node = walk->node;
  if (!node || !(css_kind(walk->parent_kind)->flags & CSS_KIND_LIST_ANY)) {
    return false;
  }
  const bool qualified_block = node->kind == GTEXT_CSS_RULE_QUALIFIED;   /* a style rule: `rules`; an at-rule: `contents` */
  const size_t n = qualified_block ? node->rule_count : node->content_count;
  while (walk->index < n) {
    const size_t i = walk->index++;
    memset(out, 0, sizeof *out);
    if (qualified_block) {
      const GTEXT_CSS_Rule * r = &node->rules[i];
      const GTEXT_CSSOM_Rule_Kind kind = css_kind_of_node(r, walk->parent_kind, false);
      if (member_of(walk->parent_kind, kind)) {
        out->kind = kind;
        out->node = r;
        return true;
      }
      continue;
    }
    const GTEXT_CSS_Block_Item * item = &node->contents[i];
    if (item->kind == GTEXT_CSS_ITEM_RULE) {
      const GTEXT_CSSOM_Rule_Kind kind = css_kind_of_node(item->rule, walk->parent_kind, false);
      if (member_of(walk->parent_kind, kind)) {
        out->kind = kind;
        out->node = item->rule;
        return true;
      }
      continue;
    }
    /* a run of declarations: in a grouping rule inside a style rule it is a nested-declarations rule (CSS Nesting), and directly in an `@scope` it is one too (CSS Cascade 6: they are the
       scope root's declarations); anywhere else it is no rule */
    if ((walk->nested || walk->parent_kind == GTEXT_CSSOM_RULE_SCOPE) && (css_kind(walk->parent_kind)->flags & CSS_KIND_LIST_RULES) && item->declaration_count > 0) {
      out->kind = GTEXT_CSSOM_RULE_NESTED_DECLARATIONS;
      out->run = item->declarations;
      out->run_count = item->declaration_count;
      return true;
    }
  }
  return false;
}
