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
 * The rule, the sheet, the rule list and the source of a parse (css_cssom_rule.h, css_cssom_sheet.h). Private.
 *
 * **The list is an order-statistics treap, and its nodes are the rules.** A rule is in at most one list at a time, so the node is part of the rule: linking a rule
 * allocates nothing (which is what lets an insertion be atomic: everything that can fail is done before the link). The priority of a node is a hash of the rule's id, so
 * the shape of a list depends on the sequence of calls and on nothing else (no address, no clock). Insertion, removal, `at(i)` and `index_of` are O(log n) expected, a walk
 * is O(1) amortized per step, `length` is O(1), and none of them recurses.
 *
 * **Ownership.** The list owns its rules through their internal counts: a rule made for a list is made with the one internal count the link will keep. The child's
 * pointer to its parent is weak (`css_obj::parent`), and `parent_sheet` is derived by following the lists' owners.
 *
 * **The source.** One parse is one syntax tree and one copy of the text its spans index, shared by every rule made from it through a plain count (the objects are
 * single-writer), and charged to the document's live-bytes limit while it lives.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSSOM_RULE_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSSOM_RULE_INTERNAL_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_cssom.h>
#include <ghoti.io/text/css/css_cssom_rule.h>
#include <ghoti.io/text/css/css_cssom_sheet.h>
#include <ghoti.io/text/macros.h>

#include "cssom_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the source of a parse ------------------------------------------------------------------------------------------------------------------- */

/// One parse: its syntax tree (retained), the copy of the text its spans index (absent after a compaction), and the id of both.
typedef struct css_source {
  size_t refs;               ///< The rules that hold it, and the sheet whose text it is.
  css_core * core;
  GTEXT_CSS_Tree * tree;     ///< Retained.
  char * text;               ///< The bytes the tree's spans index (NUL-terminated), or NULL when they are not kept.
  size_t text_length;
  uint64_t id;               ///< From the document's counter.
  size_t bytes;              ///< What it charged to the core: the tree, the text, the struct.
} css_source;

/// A source over a tree (retained here) and a copy of the text (taken over, may be NULL). Charges the core; on failure the text is freed and nothing is held.
GTEXT_CSS_Status css_source_new(css_core * core, GTEXT_CSS_Tree * tree, char * text, size_t text_length, css_source ** out);
css_source * css_source_retain(css_source * source);
void css_source_release(css_source * source);

/* ---- the rule list -------------------------------------------------------------------------------------------------------------------------- */

typedef struct GTEXT_CSSOM_Rule css_rule;
typedef struct GTEXT_CSSOM_Sheet css_sheet;

struct GTEXT_CSSOM_Rule_List {
  css_obj * owner;           ///< The sheet or the rule whose list this is.
  css_rule * root;
  size_t count;
};

/// Link the rule at `index` (at most the length): O(log n), allocates nothing, cannot fail. The rule's internal count made for the link is the list's; its parent becomes the owner.
void css_rule_list_insert(struct GTEXT_CSSOM_Rule_List * list, size_t index, css_rule * rule);
/// Unlink the rule; its parent becomes NULL (detached, with everything under it). The list's internal count is the caller's to release.
void css_rule_list_remove(struct GTEXT_CSSOM_Rule_List * list, css_rule * rule);
css_rule * css_rule_list_at(const struct GTEXT_CSSOM_Rule_List * list, size_t index);
css_rule * css_rule_list_first(const struct GTEXT_CSSOM_Rule_List * list);
css_rule * css_rule_list_next(const css_rule * rule);
size_t css_rule_list_index_of(const css_rule * rule);
/// The depth of the treap and the number of nodes (a test of its balance).
size_t css_rule_list_depth(const struct GTEXT_CSSOM_Rule_List * list);
/// Release the internal count of every rule of the list (a dying owner's); the list is empty afterward. Iterative.
void css_rule_list_release_all(struct GTEXT_CSSOM_Rule_List * list);

/* ---- the rule ------------------------------------------------------------------------------------------------------------------------------ */

/// What the kinds table (cssom_kind_tables.h, generated) says of a kind: the flags, and the places an at-rule may be. cssom_rule_kinds.c asserts they are the table's values.
#define CSS_KIND_RULE 1u               /* a parse makes a rule object of it */
#define CSS_KIND_LIST_RULES 2u         /* its cssRules holds style, nested-declarations and at-rules */
#define CSS_KIND_LIST_KEYFRAMES 4u     /* its cssRules holds keyframe rules */
#define CSS_KIND_LIST_MARGINS 8u       /* its cssRules holds margin rules */
#define CSS_KIND_STYLE_PROPERTIES 16u  /* its style is a block of property declarations */
#define CSS_KIND_STYLE_DESCRIPTORS 32u /* its style is a block of descriptors */
#define CSS_KIND_INSERTABLE 64u        /* insertRule can make one at the top level or in a group */
#define CSS_KIND_LIST_ANY (2u | 4u | 8u)

typedef struct {
  const char * name;               ///< The kind's name.
  const char * iface;          ///< The CSSOM interface ("" for a kind no browser exposes).
  unsigned legacy_type;            ///< The legacy `type`.
  unsigned flags;                  ///< CSS_KIND_*.
  unsigned parents;                ///< Where the at-rule may be (bit 1 top, 2 group, 4 style, 8 page, 16 mixin): from the ledger of at-rules.
} css_kind_info;

const css_kind_info * css_kind(GTEXT_CSSOM_Rule_Kind kind);

struct GTEXT_CSSOM_Rule {
  css_obj obj;
  uint8_t kind;                    ///< GTEXT_CSSOM_Rule_Kind.
  uint8_t source_kind;             ///< GTEXT_CSSOM_Source_Kind.
  bool has_block;                  ///< `@layer`: the rule has a block.
  bool nested;                     ///< The nested flag: the rule is inside a style rule, at any depth.
  uint64_t version;
  /* the treap node: the rule is in the list `list` */
  css_rule * left, * right, * up;
  size_t size;                     ///< Nodes in the subtree of this rule.
  uint32_t prio;
  struct GTEXT_CSSOM_Rule_List * list;   ///< The list that holds the rule, or NULL when detached from every list.
  struct GTEXT_CSSOM_Rule_List rules;    ///< The rules inside this one (empty for a kind that has no `cssRules`).
  /* where the rule's text is */
  css_source * source;
  const GTEXT_CSS_Rule * node;     ///< The syntax node of the rule (into the source's tree), or NULL for a run.
  const GTEXT_CSS_Declaration * run;   ///< A nested-declarations rule made of a run of declarations of an at-rule's block: the run.
  size_t run_count;
  GTEXT_CSSOM_Decl * style;        ///< The declaration block, built the first time it is asked for (internal count held).
};

css_rule * css_rule_from_obj(css_obj * o);

/// A rule of `kind` for a parse: made with the one internal count a list's link keeps, charged to the core, holding a reference of `source`.
GTEXT_CSS_Status css_rule_create(css_core * core, GTEXT_CSSOM_Rule_Kind kind, css_source * source, const GTEXT_CSS_Rule * node, const GTEXT_CSS_Declaration * run, size_t run_count,
    bool nested, css_rule ** out);

/// The sheet at the top of the chain of parents, or NULL.
css_sheet * css_rule_sheet(const css_rule * rule);

/// The span of the rule in its source.
GTEXT_CSS_Span css_rule_span(const css_rule * rule);

/// The declarations the rule's own block is made of (a style rule's leading ones, a nested-declarations rule's); false for a kind with no property block here.
bool css_rule_declarations(const css_rule * rule, const GTEXT_CSS_Declaration ** declarations, size_t * count);

/* ---- children of a syntax node --------------------------------------------------------------------------------------------------------------- */

/// One thing a rule's block holds that becomes a rule: a node, or a run of declarations that becomes a nested-declarations rule.
typedef struct {
  GTEXT_CSSOM_Rule_Kind kind;
  const GTEXT_CSS_Rule * node;
  const GTEXT_CSS_Declaration * run;
  size_t run_count;
} css_child;

/// The walk over what a rule's block holds, in order, yielding those that become rules (the same walk makes the objects and, in a compaction, pairs the objects with the nodes of
/// a copy of the tree). `nested` is the nested flag of the children.
typedef struct {
  const GTEXT_CSS_Rule * node;
  GTEXT_CSSOM_Rule_Kind parent_kind;
  bool nested;
  size_t index;
} css_child_walk;

void css_child_walk_init(css_child_walk * walk, GTEXT_CSSOM_Rule_Kind parent_kind, const GTEXT_CSS_Rule * node, bool nested);
bool css_child_walk_next(css_child_walk * walk, css_child * out);

/// The kind of a syntax node that is a child of a rule of `parent_kind` (or of the top level, `parent_kind` UNKNOWN with `top` set).
GTEXT_CSSOM_Rule_Kind css_kind_of_node(const GTEXT_CSS_Rule * node, GTEXT_CSSOM_Rule_Kind parent_kind, bool top);

/* ---- the sheet ------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSSOM_Parse_Diag_Kind kind;
  GTEXT_CSS_Span span;
  const char * reference;
} css_parse_diag;

struct GTEXT_CSSOM_Sheet {
  css_obj obj;
  struct GTEXT_CSSOM_Rule_List rules;
  uint64_t version;
  char * href, * title, * base_url;
  const void * owner_node;
  bool disabled, alternate, origin_clean, constructed, disallow_modification;
  size_t rule_total;               ///< Rules at every depth.
  css_source * source;             ///< The parse of the sheet's text, or NULL once it was compacted.
  css_parse_diag * diags;
  size_t ndiags, capdiags, diag_total;
};

css_sheet * css_sheet_from_obj(css_obj * o);

/// Parse a sheet's text into rule objects (cssom_parse.c).
GTEXT_CSS_Status css_sheet_parse_into(css_core * core, css_sheet * sheet, char * text, size_t length);

/// Copy what is left of the sheet into one new tree (cssom_sheet.c).
GTEXT_CSS_Status css_sheet_compact(css_sheet * sheet);

/// A copy of the rule's syntax subtree in the tree being built (css_tree_copy.c).
GTEXT_CSS_Status css_tree_new_empty(const GTEXT_Allocator * alloc, GTEXT_CSS_Tree ** out);
GTEXT_CSS_Status css_tree_copy_rule(GTEXT_CSS_Tree * dst, const GTEXT_CSS_Rule * src, const GTEXT_CSS_Rule ** out);
GTEXT_CSS_Status css_tree_copy_run(GTEXT_CSS_Tree * dst, const GTEXT_CSS_Declaration * src, size_t count, const GTEXT_CSS_Declaration ** out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_RULE_INTERNAL_H
