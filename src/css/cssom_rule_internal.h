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

/* ---- the namespaces a selector is read and written in ------------------------------------------------------------------------------------- */

/// The prefixes a style rule's selectors may use and the default namespace: the `@namespace` rules of the sheet, as they were when the rule was made. Immutable and shared by plain
/// counts. A selector is written back with the environment it was read in, so that a rule keeps its text after it leaves its sheet.
typedef struct css_ns_env {
  size_t refs;
  css_core * core;
  GTEXT_CSS_Namespace * entries;   ///< Prefix and URI, NUL-terminated copies owned by `strings`.
  size_t count;
  char * default_uri;              ///< NULL: no default namespace.
  char * strings;                  ///< One block holding every string above.
  size_t bytes;                    ///< What it charged to the core.
} css_ns_env;

/// A new environment of these prefixes and this default namespace (copied; NULL for none).
GTEXT_CSS_Status css_ns_env_new(css_core * core, const GTEXT_CSS_Namespace * entries, size_t count, const char * default_uri, css_ns_env ** out);
/// A new environment holding `base`'s with `prefix` (NULL: the default namespace) bound to `uri`; `base` may be NULL. A prefix already bound is rebound.
GTEXT_CSS_Status css_ns_env_with(css_core * core, const css_ns_env * base, const char * prefix, const char * uri, css_ns_env ** out);
css_ns_env * css_ns_env_retain(css_ns_env * env);
void css_ns_env_release(css_ns_env * env);
/// The map the selector parser takes (borrowed from `env`; a NULL environment is the empty map).
void css_ns_env_map(const css_ns_env * env, GTEXT_CSS_Namespace_Map * map);

/// One parse: its syntax tree (retained), the copy of the text its spans index (absent after a compaction), and the id of both.
typedef struct css_source {
  size_t refs;               ///< The rules that hold it, and the sheet whose text it is.
  css_core * core;
  css_ns_env * ns;           ///< The namespaces its selectors were read in (retained), or NULL for none.
  GTEXT_CSS_Tree * tree;     ///< Retained.
  char * text;               ///< The bytes the tree's spans index (NUL-terminated), or NULL when they are not kept.
  size_t text_length;
  uint64_t id;               ///< From the document's counter.
  size_t bytes;              ///< What it charged to the core: the tree, the text, the struct.
} css_source;

/// A source over a tree (retained here) and a copy of the text (taken over, may be NULL). Charges the core; on failure the text is freed and nothing is held.
GTEXT_CSS_Status css_source_new(css_core * core, GTEXT_CSS_Tree * tree, char * text, size_t text_length, css_source ** out);
css_source * css_source_retain(css_source * source);
/// The namespaces the source's selectors are read in (retained here; NULL for none).
void css_source_set_ns(css_source * source, css_ns_env * env);
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
/// The order classes of the rules at the top of a sheet (CSS Cascade 5 `@import`, CSS Namespaces 3 `@namespace`): the aggregates the order constraints are decided with.
#define CSS_CLASS_IMPORT 0u
#define CSS_CLASS_NAMESPACE 1u
#define CSS_CLASS_LAYER_STATEMENT 2u
#define CSS_CLASS_OTHER 3u
unsigned css_rule_class(const css_rule * rule);
/// How many rules of each class the list holds, and how many of them come before `index` (O(log n)).
void css_rule_list_totals(const struct GTEXT_CSSOM_Rule_List * list, size_t out[4]);
void css_rule_list_prefix(const struct GTEXT_CSSOM_Rule_List * list, size_t index, size_t out[4]);
/// The `k`th rule (from 0) of an order class, in list order: O(log n).
css_rule * css_rule_list_nth_of_class(const struct GTEXT_CSSOM_Rule_List * list, unsigned cls, size_t k);
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
  uint32_t agg[4];                 ///< Rules of each order class in the subtree of this node (CSS_CLASS_*), for the order constraints of the top of a sheet in O(log n).
  size_t descendants;              ///< Rules inside this one, at every depth.
  struct css_text_cache * cache;   ///< The cached `cssText` of a leaf rule, or NULL.
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

/// The cached `cssText` of a leaf rule (cssom_rule_serialize.c): valid while `version` is the rule's.
typedef struct css_text_cache {
  char * text;
  size_t length;
  uint64_t version;
  size_t bytes;                    ///< What it charged to the core, the entry included.
  css_rule * rule;
  struct css_text_cache * prev, * next;
} css_text_cache;

/// A mutation of the rule: its version moves and the text it kept is dropped.
void css_rule_touch(css_rule * rule);
void css_rule_drop_cache(css_rule * rule);
/// Build the rule's declaration block now (a style rule or a nested-declarations rule); `rule->style` is set. For a rule that has one already, nothing happens.
GTEXT_CSS_Status css_rule_build_style(css_rule * rule);

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
  css_ns_env * ns;                 ///< The sheet's `@namespace` rules: what a selector inserted into it is read in. NULL for none.
  css_parse_diag * diags;
  size_t ndiags, capdiags, diag_total;
};

css_sheet * css_sheet_from_obj(css_obj * o);

/// Where an inserted rule goes: what its text is read in.
typedef struct {
  bool top;                              ///< The list is a sheet's.
  const GTEXT_CSS_Rule * parent_node;    ///< The syntax node of the rule whose list it is (its name and prelude), or NULL for a sheet and for a style rule without one.
  bool relative;                         ///< Some enclosing rule, at any depth, is a style rule or an `@scope` (the place's own rule included).
  bool in_scope;                         ///< The nearest of those is an `@scope`.
  bool in_style;                         ///< Some enclosing rule is a style rule (the nested flag).
  bool at_in_place;                      ///< An at-rule of the text is read in the place, not as a rule of a style sheet (the margin rules of an `@page`).
  const css_ns_env * ns;                 ///< The namespaces its selectors are read in.
} css_insert_ctx;

/// CSS Syntax's "parse a rule" over the text of an `insertRule`, with the validity of the place: a tree of exactly one rule, or `*syntax_error` (the text is not one rule, or the rule is
/// not valid there) with no tree. Resource failures and limits are the status.
GTEXT_CSS_Status css_parse_inserted(css_core * core, const css_insert_ctx * ctx, const char * text, size_t length, GTEXT_CSS_Tree ** out, bool * syntax_error);
/// CSS Syntax's "parse a list of declarations" over that text (the nested `insertRule` that is not a rule): the tree holds the declarations.
GTEXT_CSS_Status css_parse_inserted_declarations(css_core * core, const char * text, size_t length, GTEXT_CSS_Tree ** out);
/// The rule objects of an inserted rule and of everything inside it, unlinked: `parent_depth` is the depth of the list's owner (0 for a sheet), `sheet_total` the rules the sheet holds. `*made`
/// receives the number of rules made, the top one included. On failure nothing is left.
GTEXT_CSS_Status css_build_inserted(css_core * core, css_source * source, GTEXT_CSSOM_Rule_Kind kind, const GTEXT_CSS_Rule * node, const GTEXT_CSS_Declaration * run, size_t run_count,
    bool nested, size_t parent_depth, size_t sheet_total, css_rule ** out, size_t * made);

/// Parse a sheet's text into rule objects (cssom_parse.c).
GTEXT_CSS_Status css_sheet_parse_into(css_core * core, css_sheet * sheet, char * text, size_t length);

/// Copy what is left of the sheet into one new tree (cssom_sheet.c).
GTEXT_CSS_Status css_sheet_compact(css_sheet * sheet);

/// A copy of the rule's syntax subtree in the tree being built (css_tree_copy.c).
GTEXT_CSS_Status css_tree_new_empty(const GTEXT_Allocator * alloc, GTEXT_CSS_Tree ** out);
GTEXT_CSS_Status css_tree_copy_rule(GTEXT_CSS_Tree * dst, const GTEXT_CSS_Rule * src, const GTEXT_CSS_Rule ** out);
/// A copy of the rule's own syntax: without the rules inside it when `drop_rules`, and without the runs of declarations of an at-rule's block when `drop_runs` (what the rule objects under
/// it hold, each from its own node).
GTEXT_CSS_Status css_tree_copy_rule_own(GTEXT_CSS_Tree * dst, const GTEXT_CSS_Rule * src, bool drop_rules, bool drop_runs, const GTEXT_CSS_Rule ** out);
GTEXT_CSS_Status css_tree_copy_run(GTEXT_CSS_Tree * dst, const GTEXT_CSS_Declaration * src, size_t count, const GTEXT_CSS_Declaration ** out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSSOM_RULE_INTERNAL_H
