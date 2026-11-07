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
 * The parse of a style sheet into rule objects (css_cssom_sheet.h): CSS Syntax's "parse a stylesheet" with the standard's "valid in the current context" answered by the layers
 * that know (the at-rules' grammars, the selectors, the keyframe selectors, the order of `@import` and `@namespace`), and then the rule objects built top-down from the
 * syntax tree with an explicit stack.
 *
 * **Where the validity is decided.** The parser asks, for each rule when it is complete, whether it is valid where it is. An invalid rule is dropped by the parser itself,
 * as the standard says: that is what makes the declarations around an invalid nested rule one run and not two. This file answers, and keeps the answer's reasons as
 * diagnostics. The namespace prefixes a selector may use are the `@namespace` rules that were valid before it, which are met in order.
 *
 * **What is lazy.** A style rule's selector is parsed here only to decide that the rule is valid and the result is dropped; its declaration block is not built at all until it is
 * asked for. A parse and hold of a sheet is linear in its source.
 */

#include <string.h>

#include "css_atrule_internal.h"
#include "css_internal.h"
#include "css_sheet_internal.h"
#include "cssom_rule_internal.h"

typedef struct {
  GTEXT_CSS_Atrule_Validity at;       ///< The at-rules' answer (css_atrule.h).
  css_core * core;
  GTEXT_CSS_Selector_Options selector;
  GTEXT_CSS_Namespace * namespaces;
  char ** strings;                    ///< Owned copies of the prefixes and URIs of `namespaces`.
  size_t nns, capns, nstrings;
  char * default_uri;
  bool imports_open, ns_open;
  bool constructed;                   ///< The sheet is a constructed one: `@import` is not a rule of it ("replaceSync": the rules of the text, the `@import` rules ignored).
  css_parse_diag * diags;
  size_t ndiags, capdiags, diag_total;
  GTEXT_CSS_Status status;            ///< The first resource failure inside a callback (which can only answer yes or no).
  /* an inserted rule (css_parse_inserted): the one rule of the text is read in the context of the place it is inserted at */
  bool single;
  bool at_in_place;                   ///< An at-rule of the text is read in the place (the margin rules of an `@page`), not as a rule of a style sheet.
  const GTEXT_CSS_Parent * top;       ///< The parent the rule at the top of the text has: the rule it is inserted into.
  bool seed_relative, seed_scope, seed_style, seed_mixin;   ///< What the rules of the text that have no style rule or `@scope` above them in the text are in.
} cvalidity;

/// Whether an at-rule is valid or not by the place it is in, and so is read there when it is inserted: `@apply`, `@private` and `@contents` are valid in some rule bodies and nowhere else ("The
/// @apply rule is only valid in the body of a style rule or nested group rule; using it in any other context causes it to be invalid and ignored", "Outside of a mixin body, the @contents
/// rule is invalid and ignored"), and `@layer` is invalid in a mixin body. Read as a rule of a style sheet they would all be invalid, and could be inserted nowhere.
static bool depends_on_place(const GTEXT_CSS_Rule * rule, const GTEXT_CSS_Parent * top) {
  if (rule->name.length == 5 && css_ascii_ieq(&rule->name, "apply")) {
    return true;
  }
  if ((rule->name.length == 7 && css_ascii_ieq(&rule->name, "private")) || (rule->name.length == 8 && css_ascii_ieq(&rule->name, "contents"))) {
    return true;
  }
  return top && top->in_mixin && rule->name.length == 5 && css_ascii_ieq(&rule->name, "layer");
}

/// The parent as the validity of the rule is decided in: the context an inserted rule is placed in stands behind the text's own.
static GTEXT_CSS_Parent effective_parent(const cvalidity * v, const GTEXT_CSS_Parent * p, const GTEXT_CSS_Rule * rule) {
  if (!v->single) {
    return *p;
  }
  if (p->top_level) {
    /* the standard's "parse a rule" reads an at-rule as a rule of a style sheet, whatever the place it is inserted at (the place's constraints are HierarchyRequestError, after); a
       style rule is read in the place, because its selectors are (a relative selector is valid in a style rule and not in a style sheet) */
    if (rule->kind == GTEXT_CSS_RULE_AT && !v->at_in_place && !depends_on_place(rule, v->top)) {
      GTEXT_CSS_Parent sheet_top;
      memset(&sheet_top, 0, sizeof sheet_top);
      sheet_top.top_level = true;
      return sheet_top;
    }
    return *v->top;
  }
  GTEXT_CSS_Parent e = *p;
  if (!e.relative) {
    e.relative = v->seed_relative;
    e.in_scope = v->seed_scope;
  }
  e.in_style_rule = e.in_style_rule || v->seed_style;
  e.in_mixin = e.in_mixin || v->seed_mixin;
  return e;
}

static void note_failure(cvalidity * v, GTEXT_CSS_Status st) {
  if (v->status == GTEXT_CSS_OK) {
    v->status = st;
  }
}

static void diagnose(cvalidity * v, GTEXT_CSSOM_Parse_Diag_Kind kind, const GTEXT_CSS_Rule * rule, const char * reference) {
  v->diag_total++;
  if (v->ndiags >= v->core->config.max_diagnostics) {
    return;
  }
  if (v->ndiags == v->capdiags) {
    const size_t cap = v->capdiags ? v->capdiags * 2 : 16;
    css_parse_diag * grown = gtext_allocator_realloc(&v->core->alloc, v->diags, cap * sizeof *grown);
    if (!grown) {
      note_failure(v, GTEXT_CSS_E_OOM);
      return;
    }
    v->diags = grown;
    v->capdiags = cap;
  }
  v->diags[v->ndiags].kind = kind;
  v->diags[v->ndiags].span = rule->span;
  v->diags[v->ndiags].reference = reference;
  v->ndiags++;
}

/// A prefix and its URI, remembered for the selectors that follow.
static void remember_namespace(cvalidity * v, const GTEXT_CSS_String * prefix, const GTEXT_CSS_String * uri) {
  if (v->single) {
    return;   /* one rule: nothing follows it, and the namespaces it is read in are the sheet's */
  }
  char * u = css_core_dup(v->core, uri->data ? uri->data : "", uri->length);
  char * p = prefix ? css_core_dup(v->core, prefix->data, prefix->length) : NULL;
  if (!u || (prefix && !p)) {
    gtext_allocator_free(&v->core->alloc, u);
    gtext_allocator_free(&v->core->alloc, p);
    note_failure(v, GTEXT_CSS_E_OOM);
    return;
  }
  if (!p) {
    gtext_allocator_free(&v->core->alloc, v->default_uri);
    v->default_uri = u;
    return;
  }
  for (size_t i = 0; i < v->nns; i++) {
    if (strcmp(v->namespaces[i].prefix, p) == 0) {
      /* a later @namespace of a prefix replaces the earlier */
      for (size_t k = 0; k < v->nstrings; k++) {
        if (v->strings[k] == v->namespaces[i].uri) {
          gtext_allocator_free(&v->core->alloc, v->strings[k]);
          v->strings[k] = u;
        }
      }
      v->namespaces[i].uri = u;
      gtext_allocator_free(&v->core->alloc, p);
      return;
    }
  }
  if (v->nns == v->capns) {
    const size_t cap = v->capns ? v->capns * 2 : 8;
    GTEXT_CSS_Namespace * grown = gtext_allocator_realloc(&v->core->alloc, v->namespaces, cap * sizeof *grown);
    char ** sgrown = gtext_allocator_realloc(&v->core->alloc, v->strings, cap * 2 * sizeof *sgrown);
    if (grown) {
      v->namespaces = grown;
    }
    if (sgrown) {
      v->strings = sgrown;
    }
    if (!grown || !sgrown) {
      gtext_allocator_free(&v->core->alloc, u);
      gtext_allocator_free(&v->core->alloc, p);
      note_failure(v, GTEXT_CSS_E_OOM);
      return;
    }
    v->capns = cap;
  }
  v->namespaces[v->nns].prefix = p;
  v->namespaces[v->nns].uri = u;
  v->nns++;
  v->strings[v->nstrings++] = p;
  v->strings[v->nstrings++] = u;
}

static void validity_free(cvalidity * v) {
  GTEXT_Allocator a = v->core->alloc;
  if (v->single) {
    gtext_allocator_free(&a, v->diags);   /* the namespaces are the sheet's: borrowed */
    return;
  }
  for (size_t i = 0; i < v->nstrings; i++) {
    gtext_allocator_free(&a, v->strings[i]);
  }
  gtext_allocator_free(&a, v->strings);
  gtext_allocator_free(&a, v->namespaces);
  gtext_allocator_free(&a, v->default_uri);
  gtext_allocator_free(&a, v->diags);
}

static GTEXT_CSSOM_Rule_Kind parent_kind_of(const GTEXT_CSS_Parent * parent) {
  if (parent->top_level) {
    return GTEXT_CSSOM_RULE_UNKNOWN;
  }
  if (parent->kind == GTEXT_CSS_RULE_QUALIFIED) {
    return GTEXT_CSSOM_RULE_STYLE;
  }
  const void * row = css_atrule_row_find(parent->name.data, parent->name.length);
  return row ? (GTEXT_CSSOM_Rule_Kind)css_atrule_row_kind(row) : GTEXT_CSSOM_RULE_UNKNOWN;
}

static bool at_rule_valid(cvalidity * v, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Rule * rule) {
  const GTEXT_CSSOM_Rule_Kind kind = css_kind_of_node(rule, parent_kind_of(parent), parent->top_level);
  if (kind == GTEXT_CSSOM_RULE_FEATURE_BLOCK) {
    return v->at.validity.rule(v->at.validity.user, parent, rule);   /* an entry of its parent's map: not a rule, kept for the parent's typed model */
  }
  if (kind == GTEXT_CSSOM_RULE_CHARSET && parent->top_level) {
    diagnose(v, GTEXT_CSSOM_PARSE_CHARSET, rule, "CSS Syntax 3, 3.2: the bytes were decoded by the @charset rule; CSSOM has no rule for it");
    return false;
  }
  if (kind == GTEXT_CSSOM_RULE_UNKNOWN) {
    diagnose(v, GTEXT_CSSOM_PARSE_UNKNOWN_AT_RULE, rule, "CSS Syntax 3, 5.4.2: an at-rule that is not recognized is invalid, and CSSOM does not make a rule of it");
    return false;
  }
  if (!v->at.validity.rule(v->at.validity.user, parent, rule)) {
    diagnose(v, GTEXT_CSSOM_PARSE_INVALID_AT_RULE, rule, "CSS Syntax 3, 5.4.2: a rule that is not valid in the current context is dropped");
    return false;
  }
  if (kind == GTEXT_CSSOM_RULE_SCOPE) {
    /* `@scope (<start>)? to (<end>)?`: the selectors in it must be valid, as the compile reads them */
    GTEXT_CSS_Namespace_Map map;
    map.entries = v->namespaces;
    map.count = v->nns;
    map.default_uri = v->default_uri;
    GTEXT_CSS_Selector_List *start = NULL, *end = NULL;
    bool bad = false;
    const GTEXT_CSS_Status st = css_sheet_scope_boundaries(&v->core->alloc, &v->selector, &map, rule->prelude, rule->prelude_count, parent->relative, parent->in_scope, &start, &end, &bad);
    gtext_css_selector_list_free(start);
    gtext_css_selector_list_free(end);
    if (st != GTEXT_CSS_OK) {
      note_failure(v, st);
      return true;
    }
    if (bad) {
      diagnose(v, GTEXT_CSSOM_PARSE_INVALID_AT_RULE, rule, "CSS Cascading 6, 2.1: the prelude of @scope is not valid: the rule is dropped");
      return false;
    }
  }
  if (!parent->top_level) {
    return true;
  }
  /* the order of the rules at the top of a sheet */
  switch (kind) {
  case GTEXT_CSSOM_RULE_IMPORT:
    if (v->constructed) {
      diagnose(v, GTEXT_CSSOM_PARSE_INVALID_AT_RULE, rule, "CSSOM, replaceSync(): \"Parse a stylesheet from text ... Ignore any @import rules\" (a constructed style sheet has none)");
      return false;
    }
    if (!v->imports_open) {
      diagnose(v, GTEXT_CSSOM_PARSE_MISPLACED_IMPORT, rule, "CSS Cascade 4, @import: any @import rules must precede all other valid at-rules and style rules in a style sheet (ignoring @charset and @layer statements)");
      return false;
    }
    return true;
  case GTEXT_CSSOM_RULE_NAMESPACE: {
    v->imports_open = false;
    const GTEXT_CSS_String * prefix = NULL;
    const GTEXT_CSS_String * uri = NULL;
    if (!v->ns_open || !css_sheet_namespace_prelude(rule, &prefix, &uri)) {
      diagnose(v, GTEXT_CSSOM_PARSE_MISPLACED_NAMESPACE, rule, "CSS Namespaces 3, 2.1: an @namespace rule follows @charset and @import rules and precedes all other rules");
      return false;
    }
    remember_namespace(v, prefix, uri);
    return true;
  }
  case GTEXT_CSSOM_RULE_LAYER:
    if (!rule->has_block) {
      return true;   /* a layer statement closes nothing */
    }
    /* fall through */
  default:
    v->imports_open = false;
    v->ns_open = false;
    return true;
  }
}

static bool qualified_rule_valid(cvalidity * v, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Rule * rule) {
  const GTEXT_CSSOM_Rule_Kind pk = parent_kind_of(parent);
  if (!parent->top_level && parent->kind == GTEXT_CSS_RULE_AT) {
    if (pk == GTEXT_CSSOM_RULE_KEYFRAMES) {
      bool valid = false;
      const GTEXT_CSS_Status st = css_atrule_keyframe_block_valid(v->core->registry, rule, &valid);
      if (st != GTEXT_CSS_OK) {
        note_failure(v, st);
        return true;
      }
      if (!valid) {
        diagnose(v, GTEXT_CSSOM_PARSE_INVALID_KEYFRAME, rule, "CSS Animations 1, 3: a keyframe selector is from, to or a percentage");
      }
      return valid;
    }
    if (!(css_kind(pk)->flags & CSS_KIND_LIST_RULES) && !v->single) {
      return true;   /* an at-rule that holds no style rules: whatever it does with them, they are not rules of its list (an inserted rule is judged as the rule it is first, then by its place) */
    }
  }
  /* a style rule: its selector list must be valid, as a relative selector list when it is nested */
  GTEXT_CSS_Namespace_Map map;
  map.entries = v->namespaces;
  map.count = v->nns;
  map.default_uri = v->default_uri;
  GTEXT_CSS_Selector_List * list = NULL;
  const GTEXT_CSS_Status st = css_sheet_selector_prelude(&v->core->alloc, &v->selector, &map, rule->prelude, rule->prelude_count, parent->relative, parent->in_scope, &list);
  if (st != GTEXT_CSS_OK) {
    note_failure(v, st);
    return true;
  }
  if (!list) {
    diagnose(v, GTEXT_CSSOM_PARSE_INVALID_SELECTOR, rule, "Selectors 4, 3.6: an invalid selector makes its rule invalid, with what is nested in it");
    return false;
  }
  gtext_css_selector_list_free(list);
  if (parent->top_level) {
    v->imports_open = false;
    v->ns_open = false;
  }
  return true;
}

static bool rule_valid(void * user, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Rule * rule) {
  cvalidity * v = user;
  if (v->status != GTEXT_CSS_OK) {
    return true;   /* the parse is going to fail: nothing is judged any more */
  }
  const GTEXT_CSS_Parent where = effective_parent(v, parent, rule);
  if (rule->kind == GTEXT_CSS_RULE_AT) {
    return at_rule_valid(v, &where, rule);
  }
  if (rule->kind == GTEXT_CSS_RULE_QUALIFIED) {
    return qualified_rule_valid(v, &where, rule);
  }
  return true;
}

/* ---- building the objects ---------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  css_rule * rule;
  css_child_walk walk;
  size_t depth;
} frame;

typedef struct {
  css_core * core;
  css_source * source;
  size_t * total;                  ///< Rules made so far in the sheet (or in the call, for an inserted rule): counted against `max_rules`.
  size_t total_base;               ///< Rules the sheet holds besides those counted in `total`.
  size_t depth_base;               ///< The depth of the place the rules are made for: the rules made are at least one deeper.
  frame * stack;
  size_t sp, cap;
  size_t work;
  uint8_t source_kind;             ///< GTEXT_CSSOM_Source_Kind of the rules made.
} builder;

static GTEXT_CSS_Status push_frame(builder * b, css_rule * r, size_t depth) {
  if (b->sp == b->cap) {
    const size_t cap = b->cap ? b->cap * 2 : 16;
    frame * grown = gtext_allocator_realloc(&b->core->alloc, b->stack, cap * sizeof *grown);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    b->stack = grown;
    b->cap = cap;
  }
  frame * f = &b->stack[b->sp++];
  f->rule = r;
  f->depth = depth;
  css_child_walk_init(&f->walk, (GTEXT_CSSOM_Rule_Kind)r->kind, r->node, r->nested || r->kind == GTEXT_CSSOM_RULE_STYLE);
  return GTEXT_CSS_OK;
}

/// Account a new rule against the limits of the sheet and of the call.
static GTEXT_CSS_Status account(builder * b, size_t depth) {
  if (b->depth_base + depth > b->core->config.max_rule_nesting) {
    return GTEXT_CSS_E_LIMIT_RULE_NESTING;
  }
  if (b->total_base + *b->total >= b->core->config.max_rules) {
    return GTEXT_CSS_E_LIMIT_RULES;
  }
  if (++b->work > b->core->config.max_work) {
    return GTEXT_CSS_E_LIMIT_WORK;
  }
  ++*b->total;
  return GTEXT_CSS_OK;
}

/// The rule objects of the subtree of a rule that has just been made, built with the explicit stack and linked under it. Each rule's `descendants` is complete when it returns.
static GTEXT_CSS_Status build_below(builder * b, css_rule * top) {
  GTEXT_CSS_Status st = push_frame(b, top, 1);
  while (st == GTEXT_CSS_OK && b->sp) {
    frame * f = &b->stack[b->sp - 1];
    css_child c;
    if (!css_child_walk_next(&f->walk, &c)) {
      css_rule * done = f->rule;
      b->sp--;
      if (b->sp) {
        b->stack[b->sp - 1].rule->descendants += done->descendants;
      }
      continue;
    }
    st = account(b, f->depth + 1);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    css_rule * r = NULL;
    st = css_rule_create(b->core, c.kind, b->source, c.node, c.run, c.run_count, f->walk.nested, &r);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    r->source_kind = b->source_kind;
    css_rule_list_insert(&f->rule->rules, f->rule->rules.count, r);
    f->rule->descendants++;
    if (css_kind(c.kind)->flags & CSS_KIND_LIST_ANY) {
      const size_t depth = f->depth + 1;   /* f may move when the stack grows */
      st = push_frame(b, r, depth);
    }
  }
  return st;
}

GTEXT_CSS_Status css_build_inserted(css_core * core, css_source * source, GTEXT_CSSOM_Rule_Kind kind, const GTEXT_CSS_Rule * node, const GTEXT_CSS_Declaration * run, size_t run_count,
    bool nested, size_t parent_depth, size_t sheet_total, css_rule ** out, size_t * made) {
  size_t total = 0;
  builder b;
  memset(&b, 0, sizeof b);
  b.core = core;
  b.source = source;
  b.total = &total;
  b.total_base = sheet_total;
  b.depth_base = parent_depth;
  b.source_kind = GTEXT_CSSOM_SOURCE_INSERTED;
  GTEXT_CSS_Status st = account(&b, 1);
  css_rule * top = NULL;
  if (st == GTEXT_CSS_OK) {
    st = css_rule_create(core, kind, source, node, run, run_count, nested, &top);
  }
  if (top) {
    top->source_kind = GTEXT_CSSOM_SOURCE_INSERTED;
  }
  if (st == GTEXT_CSS_OK && (css_kind(kind)->flags & CSS_KIND_LIST_ANY)) {
    st = build_below(&b, top);
  }
  gtext_allocator_free(&core->alloc, b.stack);
  if (st != GTEXT_CSS_OK) {
    if (top) {
      css_obj_release_internal(&top->obj);   /* frees it with whatever was built under it */
    }
    return st;
  }
  *out = top;
  *made = total;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_sheet_parse_into(css_core * core, css_sheet * sheet, char * text, size_t length) {
  GTEXT_Allocator a = core->alloc;
  cvalidity v;
  memset(&v, 0, sizeof v);
  v.core = core;
  v.imports_open = true;
  v.ns_open = true;
  v.constructed = sheet->constructed;
  v.selector = gtext_css_selector_options_default();
  gtext_css_atrule_validity_init(&v.at, core->registry);
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &a;
  po.max_input_bytes = core->config.max_input_bytes;
  po.max_rules = core->config.max_rules;
  po.max_depth = core->config.max_rule_nesting + 512;
  po.count_errors_only = true;
  po.validity.user = &v;
  po.validity.rule = rule_valid;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_stylesheet(text ? text : "", length, &po, &tree, NULL);
  if (st == GTEXT_CSS_OK && v.status != GTEXT_CSS_OK) {
    st = v.status;
    gtext_css_tree_free(tree);
    tree = NULL;
  }
  if (st != GTEXT_CSS_OK) {
    validity_free(&v);
    gtext_allocator_free(&a, text);
    return st;
  }
  css_source * source = NULL;
  st = css_source_new(core, tree, text, length, &source);   /* takes the text over, even when it fails */
  gtext_css_tree_free(tree);
  if (st != GTEXT_CSS_OK) {
    validity_free(&v);
    return st;
  }
  sheet->source = source;
  if (v.nns || v.default_uri) {
    /* the namespaces every style rule of this parse is read in: the `@namespace` rules precede all others, so the last state of the list is every rule's */
    css_ns_env * env = NULL;
    st = css_ns_env_new(core, v.namespaces, v.nns, v.default_uri, &env);
    if (st != GTEXT_CSS_OK) {
      validity_free(&v);
      return st;
    }
    css_source_set_ns(source, env);
    sheet->ns = env;
  }
  builder b;
  memset(&b, 0, sizeof b);
  b.core = core;
  b.source = source;
  b.total = &sheet->rule_total;
  /* the top-level rules, in order; the parse has dropped everything that is not a rule here */
  for (size_t i = 0; st == GTEXT_CSS_OK && i < source->tree->rule_count; i++) {
    const GTEXT_CSS_Rule * node = &source->tree->rules[i];
    const GTEXT_CSSOM_Rule_Kind kind = css_kind_of_node(node, GTEXT_CSSOM_RULE_UNKNOWN, true);
    if (!(css_kind(kind)->flags & CSS_KIND_RULE)) {
      continue;
    }
    st = account(&b, 1);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    css_rule * r = NULL;
    st = css_rule_create(core, kind, source, node, NULL, 0, false, &r);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    css_rule_list_insert(&sheet->rules, sheet->rules.count, r);
    if (css_kind(kind)->flags & CSS_KIND_LIST_ANY) {
      st = build_below(&b, r);
    }
  }
  gtext_allocator_free(&a, b.stack);
  if (st == GTEXT_CSS_OK) {
    sheet->diags = v.diags;
    sheet->ndiags = v.ndiags;
    sheet->capdiags = v.capdiags;
    sheet->diag_total = v.diag_total;
    v.diags = NULL;
  }
  validity_free(&v);
  return st;
}

/* ---- one rule, for insertRule --------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status css_parse_inserted(css_core * core, const css_insert_ctx * ctx, const char * text, size_t length, GTEXT_CSS_Tree ** out, bool * syntax_error) {
  *out = NULL;
  *syntax_error = false;
  if (length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  GTEXT_Allocator a = core->alloc;
  GTEXT_CSS_Parent top;
  memset(&top, 0, sizeof top);
  top.top_level = ctx->top;
  top.kind = ctx->parent_node ? ctx->parent_node->kind : GTEXT_CSS_RULE_QUALIFIED;
  if (ctx->parent_node) {
    top.name = ctx->parent_node->name;
    top.prelude = ctx->parent_node->prelude;
    top.prelude_count = ctx->parent_node->prelude_count;
  }
  top.nested = ctx->in_style;
  top.depth = ctx->top ? 0 : 1;
  top.relative = ctx->relative;
  top.in_scope = ctx->in_scope;
  top.in_style_rule = ctx->in_style;
  top.in_mixin = ctx->in_mixin;
  cvalidity v;
  memset(&v, 0, sizeof v);
  v.core = core;
  v.single = true;
  v.at_in_place = ctx->at_in_place;
  v.top = &top;
  v.seed_relative = ctx->relative;
  v.seed_scope = ctx->in_scope;
  v.seed_style = ctx->in_style;
  v.seed_mixin = ctx->in_mixin;
  v.imports_open = true;
  v.ns_open = true;
  v.selector = gtext_css_selector_options_default();
  GTEXT_CSS_Namespace_Map map;
  css_ns_env_map(ctx->ns, &map);
  v.namespaces = (GTEXT_CSS_Namespace *)(uintptr_t)map.entries;
  v.nns = map.count;
  v.default_uri = (char *)(uintptr_t)map.default_uri;
  gtext_css_atrule_validity_init(&v.at, core->registry);
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &a;
  po.max_input_bytes = core->config.max_input_bytes;
  po.max_rules = core->config.max_rules;
  po.max_depth = core->config.max_rule_nesting + 512;
  po.count_errors_only = true;
  po.validity.user = &v;
  po.validity.rule = rule_valid;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_rule(text ? text : "", length, &po, &tree, NULL);
  if (st == GTEXT_CSS_OK && v.status != GTEXT_CSS_OK) {
    st = v.status;
  }
  validity_free(&v);
  if (st != GTEXT_CSS_OK) {
    gtext_css_tree_free(tree);
    return st;
  }
  if (tree->syntax_error != GTEXT_CSS_SYNTAX_NONE || tree->rule_count != 1) {
    gtext_css_tree_free(tree);
    *syntax_error = true;
    return GTEXT_CSS_OK;
  }
  *out = tree;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_parse_inserted_declarations(css_core * core, const char * text, size_t length, GTEXT_CSS_Tree ** out) {
  *out = NULL;
  if (length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  GTEXT_Allocator a = core->alloc;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &a;
  po.max_input_bytes = core->config.max_input_bytes;
  po.max_declarations = core->config.max_declarations;
  po.count_errors_only = true;
  return gtext_css_parse_declaration_list(text ? text : "", length, &po, out, NULL);
}
