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
 * `insertRule`, `deleteRule` and their kin (css_cssom_sheet.h, css_cssom_rule.h): CSSOM's "insert a CSS rule" and "remove a CSS rule", the legacy `addRule` and `removeRule`, and
 * the keyframes rule's `appendRule`, `deleteRule` and `findRule`.
 *
 * **Atomic.** A call parses and validates first, builds the new rule objects unlinked, and only then links them: everything that can fail comes before the first change to the
 * sheet, and a failure releases what was built and puts the id counter back, so the sheet, its versions, its ids and its observers' view are as they were.
 *
 * **The order of checks is part of the contract** (css_cssom_sheet.h gives each list). The order constraints of the top of a sheet are decided from counts kept in the rule list
 * (cssom_rule_list.c), so an insertion or a deletion is O(log n), never a walk.
 */

#include <string.h>

#include "css_atrule_internal.h"
#include "css_internal.h"
#include "css_shorthand_internal.h"
#include "css_sheet_internal.h"
#include "cssom_kind_tables.h"
#include "cssom_doc_internal.h"
#include "cssom_rule_internal.h"
#include "cssom_typed.h"

/* ---- the place a rule is inserted into ------------------------------------------------------------------------------------------------------------- */

typedef struct {
  css_core * core;
  css_sheet * sheet;               ///< The sheet the list belongs to, directly or through the rules above it; NULL when the list is detached.
  css_rule * target;               ///< The rule whose list this is; NULL for the top of a sheet.
  struct GTEXT_CSSOM_Rule_List * list;
  css_insert_ctx ctx;
  unsigned place;                  ///< CSSOM_PARENT_TOP, _GROUP, _STYLE or _PAGE: where an at-rule is allowed.
  size_t depth;                    ///< Rules above the list, the target included (0 for a sheet).
  size_t sheet_total;              ///< Rules the sheet holds, or the chain's, for the limit on how many there are.
} place;

static css_rule * owner_rule(const css_rule * rule) {
  return rule->list && rule->list->owner->kind == GTEXT_CSSOM_OBJECT_RULE ? (css_rule *)(uintptr_t)rule->list->owner : NULL;
}

static void place_for_sheet(css_sheet * sheet, place * pl) {
  memset(pl, 0, sizeof *pl);
  pl->core = sheet->obj.core;
  pl->sheet = sheet;
  pl->list = &sheet->rules;
  pl->ctx.top = true;
  pl->ctx.ns = sheet->ns;
  pl->place = CSSOM_PARENT_TOP;
  pl->sheet_total = sheet->rule_total;
}

static void place_for_rule(css_rule * target, place * pl) {
  memset(pl, 0, sizeof *pl);
  pl->core = target->obj.core;
  pl->target = target;
  pl->list = &target->rules;
  pl->ctx.parent_node = target->node;
  pl->ctx.at_in_place = target->kind == GTEXT_CSSOM_RULE_PAGE;
  bool nearest_seen = false;
  css_rule * top = target;
  for (css_rule * x = target; x; x = owner_rule(x)) {
    pl->depth++;
    top = x;
    if (x->kind == GTEXT_CSSOM_RULE_STYLE) {
      pl->ctx.in_style = true;
      if (!nearest_seen) {
        pl->ctx.relative = true;
        pl->ctx.in_scope = false;
        nearest_seen = true;
      }
    }
    else if (x->kind == GTEXT_CSSOM_RULE_SCOPE && !nearest_seen) {
      pl->ctx.relative = true;
      pl->ctx.in_scope = true;
      nearest_seen = true;
    }
  }
  pl->sheet = css_rule_sheet(target);
  pl->ctx.ns = pl->sheet ? pl->sheet->ns : (target->source ? target->source->ns : NULL);
  pl->sheet_total = pl->sheet ? pl->sheet->rule_total : top->descendants + 1;
  pl->place = target->kind == GTEXT_CSSOM_RULE_PAGE ? CSSOM_PARENT_PAGE : (pl->ctx.in_style ? CSSOM_PARENT_STYLE : CSSOM_PARENT_GROUP);
}

void css_rule_own_ctx(const css_rule * rule, css_insert_ctx * ctx, GTEXT_CSSOM_Rule_Kind * parent_kind, GTEXT_CSS_Rule * synthetic) {
  place pl;
  *parent_kind = GTEXT_CSSOM_RULE_UNKNOWN;
  if (rule->list) {
    if (rule->list->owner->kind == GTEXT_CSSOM_OBJECT_SHEET) {
      place_for_sheet((css_sheet *)(uintptr_t)rule->list->owner, &pl);
    }
    else {
      css_rule * owner = (css_rule *)(uintptr_t)rule->list->owner;
      place_for_rule(owner, &pl);
      *parent_kind = (GTEXT_CSSOM_Rule_Kind)owner->kind;
    }
    *ctx = pl.ctx;
    return;
  }
  /* in no list: the place is rebuilt from the kind (a keyframe is read in a `@keyframes`, a margin rule in a `@page`) */
  memset(ctx, 0, sizeof *ctx);
  memset(synthetic, 0, sizeof *synthetic);
  ctx->ns = rule->source ? rule->source->ns : NULL;
  if (rule->kind == GTEXT_CSSOM_RULE_KEYFRAME || rule->kind == GTEXT_CSSOM_RULE_PAGE_MARGIN) {
    const bool key = rule->kind == GTEXT_CSSOM_RULE_KEYFRAME;
    synthetic->kind = GTEXT_CSS_RULE_AT;
    synthetic->name.data = key ? "keyframes" : "page";
    synthetic->name.length = key ? 9 : 4;
    synthetic->has_block = true;
    ctx->parent_node = synthetic;
    ctx->at_in_place = !key;
    *parent_kind = key ? GTEXT_CSSOM_RULE_KEYFRAMES : GTEXT_CSSOM_RULE_PAGE;
    return;
  }
  ctx->top = !rule->nested;
  ctx->in_style = rule->nested;
  ctx->relative = rule->nested;
  if (rule->nested) {
    *parent_kind = GTEXT_CSSOM_RULE_STYLE;
  }
}

/* ---- the order constraints of the top of a sheet ---------------------------------------------------------------------------------------------------- */

/// "Insert a CSS rule": "If new rule cannot be inserted into list at the zero-indexed position index due to constraints specified by CSS, then throw a HierarchyRequestError",
/// and then InvalidStateError for an `@namespace` into a list that holds anything but `@import` and `@namespace` rules. The constraints are CSS Cascade 5 (`@import` must precede all
/// other valid at-rules and style rules, ignoring `@charset` and `@layer` statements) and CSS Namespaces 3 (`@namespace` follows `@import` and precedes all other rules).
static GTEXT_CSS_Status check_order(const place * pl, GTEXT_CSSOM_Rule_Kind kind, bool layer_block, size_t index) {
  size_t before[4], total[4];
  css_rule_list_prefix(pl->list, index, before);
  css_rule_list_totals(pl->list, total);
  const size_t after_imports = total[CSS_CLASS_IMPORT] - before[CSS_CLASS_IMPORT];
  const size_t after_namespaces = total[CSS_CLASS_NAMESPACE] - before[CSS_CLASS_NAMESPACE];
  switch (kind) {
  case GTEXT_CSSOM_RULE_IMPORT:
    return before[CSS_CLASS_NAMESPACE] + before[CSS_CLASS_OTHER] > 0 ? GTEXT_CSS_E_HIERARCHY : GTEXT_CSS_OK;
  case GTEXT_CSSOM_RULE_NAMESPACE:
    if (before[CSS_CLASS_OTHER] > 0 || after_imports > 0) {
      return GTEXT_CSS_E_HIERARCHY;
    }
    return total[CSS_CLASS_LAYER_STATEMENT] + total[CSS_CLASS_OTHER] > 0 ? GTEXT_CSS_E_INVALID_STATE : GTEXT_CSS_OK;
  case GTEXT_CSSOM_RULE_LAYER:
    if (!layer_block && before[CSS_CLASS_IMPORT] + before[CSS_CLASS_NAMESPACE] + before[CSS_CLASS_OTHER] == 0) {
      return GTEXT_CSS_OK;   /* among the statements that begin the sheet, ahead of every import and namespace (CSS Cascade 5: ignored by the constraint on `@import`) */
    }
    /* any other place: a rule that must follow the imports and the namespaces (Chromium 156 and Firefox 158 refuse a statement between an `@import` and an `@namespace`) */
    /* fall through */
  default:
    return after_imports + after_namespaces > 0 ? GTEXT_CSS_E_HIERARCHY : GTEXT_CSS_OK;
  }
}

/* ---- the namespaces of a sheet --------------------------------------------------------------------------------------------------------------------- */

/// The environment of the sheet's `@namespace` rules without `skip` (the rule being deleted): rebuilt, since a prefix an earlier rule bound may be what a later one rebound.
static GTEXT_CSS_Status rebuild_namespaces(css_core * core, css_sheet * sheet, const css_rule * skip, css_ns_env ** out) {
  GTEXT_Allocator a = core->alloc;
  size_t totals[4];
  css_rule_list_totals(&sheet->rules, totals);
  const size_t n = totals[CSS_CLASS_NAMESPACE];
  GTEXT_CSS_Namespace * entries = gtext_allocator_calloc(&a, n ? n : 1, sizeof *entries);
  char ** owned = gtext_allocator_calloc(&a, n ? 2 * n : 1, sizeof *owned);
  char * default_uri = NULL;
  size_t count = 0, nowned = 0, work = 0;
  GTEXT_CSS_Status st = entries && owned ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  for (size_t k = 0; st == GTEXT_CSS_OK && k < n; k++) {
    const css_rule * r = css_rule_list_nth_of_class(&sheet->rules, CSS_CLASS_NAMESPACE, k);
    if (r == skip) {
      continue;
    }
    const GTEXT_CSS_String * prefix = NULL;
    const GTEXT_CSS_String * uri = NULL;
    if (!css_sheet_namespace_prelude(r->node, &prefix, &uri)) {
      continue;
    }
    char * u = css_core_dup(core, uri->data ? uri->data : "", uri->length);
    char * p = prefix ? css_core_dup(core, prefix->data, prefix->length) : NULL;
    if (!u || (prefix && !p)) {
      gtext_allocator_free(&a, u);
      gtext_allocator_free(&a, p);
      st = GTEXT_CSS_E_OOM;
      break;
    }
    owned[nowned++] = u;
    if (p) {
      owned[nowned++] = p;
    }
    if (!p) {
      default_uri = u;
      continue;
    }
    bool replaced = false;
    for (size_t i = 0; i < count; i++) {
      work++;
      if (strcmp(entries[i].prefix, p) == 0) {
        entries[i].uri = u;
        replaced = true;
      }
    }
    if (!replaced) {
      entries[count].prefix = p;
      entries[count].uri = u;
      count++;
    }
    if (work > core->config.max_work) {
      st = GTEXT_CSS_E_LIMIT_WORK;
    }
  }
  if (st == GTEXT_CSS_OK) {
    st = css_ns_env_new(core, entries, count, default_uri, out);
  }
  for (size_t i = 0; i < nowned; i++) {
    gtext_allocator_free(&a, owned[i]);
  }
  gtext_allocator_free(&a, owned);
  gtext_allocator_free(&a, entries);
  return st;
}

/// The environment of the sheet's with the `@namespace` rule `node` added.
static GTEXT_CSS_Status add_namespace(css_core * core, const css_ns_env * base, const GTEXT_CSS_Rule * node, css_ns_env ** out) {
  GTEXT_Allocator a = core->alloc;
  const GTEXT_CSS_String * prefix = NULL;
  const GTEXT_CSS_String * uri = NULL;
  if (!css_sheet_namespace_prelude(node, &prefix, &uri)) {
    return GTEXT_CSS_E_INTERNAL;   /* it was valid when it was parsed */
  }
  char * u = css_core_dup(core, uri->data ? uri->data : "", uri->length);
  char * p = prefix ? css_core_dup(core, prefix->data, prefix->length) : NULL;
  GTEXT_CSS_Status st = u && (!prefix || p) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  if (st == GTEXT_CSS_OK) {
    st = css_ns_env_with(core, base, p, u, out);
  }
  gtext_allocator_free(&a, u);
  gtext_allocator_free(&a, p);
  return st;
}

/* ---- insert -------------------------------------------------------------------------------------------------------------------------------------------------- */

/// A rule is linked or unlinked: the counts above it, the versions and the one notification. `count` is the number of rules linked (positive) or unlinked (negative as `removed`).
static void settle(place * pl, css_rule * rule, size_t count, bool inserted, css_cssom_event * ev) {
  css_sheet * sheet = pl->sheet;
  for (css_rule * x = pl->target; x; x = owner_rule(x)) {
    if (inserted) {
      x->descendants += count;
    }
    else {
      x->descendants -= count;
    }
    css_rule_touch(x);
  }
  if (sheet) {
    if (inserted) {
      sheet->rule_total += count;
    }
    else {
      sheet->rule_total -= count;
    }
    css_sheet_bump(sheet);
    ev->sheet = sheet;
    ev->sheet_id = sheet->obj.id;
  }
  else {
    ev->silent = true;   /* a rule that no sheet holds changed: nothing that evaluates the document can see it */
  }
  ev->rule = rule;
  ev->rule_id = rule->obj.id;
}

static GTEXT_CSS_Status insert_text(place * pl, const char * text, size_t length, size_t index, bool keyframe, GTEXT_CSSOM_Rule ** out, GTEXT_CSSOM_Outcome * outcome) {
  css_core * core = pl->core;
  uint64_t saved_id = core->next_id;
  css_sheet * made_sheet = NULL;   /* the sheet the import callback gave for an `@import`, held until it is attached or refused */
  bool import_pending = false;
  GTEXT_CSS_Tree * tree = NULL;
  css_source * source = NULL;
  css_rule * rule = NULL;
  css_ns_env * new_env = NULL;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  bool syntax = false;
  bool declarations_rule = false;
  GTEXT_CSSOM_Rule_Kind kind = GTEXT_CSSOM_RULE_UNKNOWN;
  const GTEXT_CSS_Rule * node = NULL;
  const GTEXT_CSS_Declaration * run = NULL;
  size_t run_count = 0;
  size_t made = 0;
  const size_t count = pl->list->count;

  if (pl->ctx.top) {
    /* a sheet: the text is parsed first ("insertRule": "Let parsed rule be the return value of invoking parse a rule with rule. If parsed rule is a syntax error, throw a
       SyntaxError"), and only then is the index looked at */
    st = css_parse_inserted(core, &pl->ctx, text, length, &tree, &syntax);
    if (st != GTEXT_CSS_OK) {
      goto fail;
    }
    if (syntax) {
      st = GTEXT_CSS_E_SYNTAX;
      goto fail;
    }
    node = &tree->rules[0];
    kind = css_kind_of_node(node, GTEXT_CSSOM_RULE_UNKNOWN, true);
    if (!(css_kind(kind)->flags & CSS_KIND_RULE)) {
      st = GTEXT_CSS_E_SYNTAX;
      goto fail;
    }
    if (kind == GTEXT_CSSOM_RULE_IMPORT && pl->sheet->constructed) {
      st = GTEXT_CSS_E_SYNTAX;   /* "If parsed rule is an @import rule, and the constructed flag is set, throw a SyntaxError" */
      goto fail;
    }
    if (index > count) {
      st = GTEXT_CSS_E_INDEX_SIZE;
      goto fail;
    }
  }
  else {
    /* a rule's list ("insert a CSS rule" with the nested flag): "If index is greater than length, then throw an IndexSizeError" comes before the text is looked at */
    if (index > count) {
      st = GTEXT_CSS_E_INDEX_SIZE;
      goto fail;
    }
    st = css_parse_inserted(core, &pl->ctx, text, length, &tree, &syntax);
    if (st != GTEXT_CSS_OK) {
      goto fail;
    }
    if (syntax && keyframe) {
      /* `appendRule` ignores what is not one keyframe: no exception */
      if (outcome) {
        outcome->kind = GTEXT_CSSOM_IGNORED_INVALID_SELECTOR;
        outcome->detail = "CSS Animations 1, appendRule: text that is not one keyframe is ignored";
      }
      gtext_css_tree_free(tree);
      return GTEXT_CSS_OK;
    }
    if (syntax) {
      if (!pl->ctx.in_style && !(pl->target && pl->target->kind == GTEXT_CSSOM_RULE_SCOPE)) {
        st = GTEXT_CSS_E_SYNTAX;   /* a declaration block is a rule only in a style rule's nesting: "nested declarations rule" */
        goto fail;
      }
      st = css_parse_inserted_declarations(core, text, length, &tree);
      if (st != GTEXT_CSS_OK) {
        goto fail;
      }
      if (tree->declaration_count == 0) {
        st = GTEXT_CSS_E_SYNTAX;
        goto fail;
      }
      declarations_rule = true;
      kind = GTEXT_CSSOM_RULE_NESTED_DECLARATIONS;
      run = tree->declarations;
      run_count = tree->declaration_count;
    }
    else {
      node = &tree->rules[0];
      kind = keyframe ? css_kind_of_node(node, GTEXT_CSSOM_RULE_KEYFRAMES, false) : css_kind_of_node(node, GTEXT_CSSOM_RULE_UNKNOWN, true);
      if (!(css_kind(kind)->flags & CSS_KIND_RULE) || (keyframe && kind != GTEXT_CSSOM_RULE_KEYFRAME)) {
        if (keyframe) {
          if (outcome) {
            outcome->kind = GTEXT_CSSOM_IGNORED_INVALID_SELECTOR;
            outcome->detail = "CSS Animations 1, appendRule: text that is not one keyframe is ignored";
          }
          gtext_css_tree_free(tree);
          return GTEXT_CSS_OK;
        }
        st = GTEXT_CSS_E_SYNTAX;
        goto fail;
      }
    }
  }

  /* an `@layer` statement is not a rule of a style rule's block at all (the browsers drop it there when they read a sheet, and refuse it as a SyntaxError here) */
  if (!keyframe && !declarations_rule && kind == GTEXT_CSSOM_RULE_LAYER && !node->has_block && pl->place == CSSOM_PARENT_STYLE) {
    st = GTEXT_CSS_E_SYNTAX;
    goto fail;
  }
  /* the constraints of the place: a rule that CSS does not allow here (an `@import` in a grouping rule, an `@font-face` in a style rule) */
  if (!keyframe && !declarations_rule) {
    const bool style_like = kind == GTEXT_CSSOM_RULE_STYLE;
    if (style_like ? pl->place == CSSOM_PARENT_PAGE : !(css_kind(kind)->parents & pl->place)) {
      st = GTEXT_CSS_E_HIERARCHY;
      goto fail;
    }
  }
  if (pl->ctx.top) {
    st = check_order(pl, kind, node->has_block, index);
    if (st != GTEXT_CSS_OK) {
      goto fail;
    }
    if (kind == GTEXT_CSSOM_RULE_NAMESPACE) {
      st = add_namespace(core, pl->sheet->ns, node, &new_env);
      if (st != GTEXT_CSS_OK) {
        goto fail;
      }
    }
  }

  /* everything that can fail from here on is before the first change */
  st = css_source_new(core, tree, NULL, 0, &source);
  if (st != GTEXT_CSS_OK) {
    goto fail;
  }
  css_source_set_ns(source, (css_ns_env *)(uintptr_t)pl->ctx.ns);
  st = css_build_inserted(core, source, kind, node, run, run_count, pl->ctx.in_style, pl->depth, pl->sheet_total, &rule, &made);
  if (st != GTEXT_CSS_OK) {
    goto fail;
  }
  if (declarations_rule) {
    st = css_rule_build_style(rule);
    if (st != GTEXT_CSS_OK) {
      goto fail;
    }
    if (gtext_css_block_count(rule->style->block) == 0) {
      st = GTEXT_CSS_E_SYNTAX;   /* "If declarations is empty, throw a SyntaxError": every declaration was invalid */
      goto fail;
    }
  }

  if (kind == GTEXT_CSSOM_RULE_IMPORT && pl->ctx.top && !keyframe && !declarations_rule) {
    /* the document's import callback is asked last, when nothing else can fail; the rule is not in the sheet yet */
    bool asked = false, nesting = false;
    if (!css_sheet_diag_reserve(pl->sheet, 1)) {
      st = GTEXT_CSS_E_OOM;
      goto fail;
    }
    const uint64_t before = core->next_id;
    st = css_import_ask(pl->sheet, rule, &asked, &made_sheet, &import_pending, &nesting);
    if (st != GTEXT_CSS_OK) {
      if (core->next_id != before) {
        saved_id = core->next_id;   /* the callback made objects: their ids stay taken */
      }
      goto fail;
    }
    if (nesting || (made_sheet && css_import_check(pl->sheet, made_sheet) != GTEXT_CSS_OK)) {
      css_sheet_diag_push(pl->sheet, GTEXT_CSSOM_PARSE_IMPORT_REFUSED, css_rule_span(rule),
          nesting ? "the import callbacks are nested deeper than the library follows"
                  : "CSS Cascade 5, @import: the sheet of an import has one owner and cannot hold the sheet that holds it");
      if (made_sheet) {
        core->counters.import_refused++;
        gtext_cssom_sheet_release(made_sheet);
        made_sheet = NULL;
      }
      import_pending = false;
    }
  }

  /* the first change: nothing below can fail */
  {
    css_cssom_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = GTEXT_CSSOM_EVENT_RULE_INSERTED;
    css_rule_list_insert(pl->list, index, rule);
    settle(pl, rule, made, true, &ev);
    if (new_env) {
      css_ns_env_release(pl->sheet->ns);
      pl->sheet->ns = new_env;
      new_env = NULL;
    }
    core->counters.rule_inserts += made;
    if (made_sheet) {
      css_import_attach(rule, made_sheet);
      gtext_cssom_sheet_release(made_sheet);   /* the callback's reference: the rule holds the sheet now */
      made_sheet = NULL;
    }
    rule->import_pending = import_pending;
    if (out) {
      css_obj_retain_external(&rule->obj);
      *out = rule;
    }
    css_source_release(source);
    gtext_css_tree_free(tree);
    if (outcome) {
      outcome->kind = GTEXT_CSSOM_CHANGED;
    }
    css_obj_retain_external(&rule->obj);   /* the delivery holds it */
    css_cssom_commit(core, &ev, outcome);
    css_obj_release_external(&rule->obj);
  }
  return GTEXT_CSS_OK;

fail:
  gtext_cssom_sheet_release(made_sheet);
  if (rule) {
    css_obj_release_internal(&rule->obj);
  }
  css_source_release(source);
  gtext_css_tree_free(tree);
  css_ns_env_release(new_env);
  core->next_id = saved_id;
  return st;
}

/* ---- delete ------------------------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status delete_at(place * pl, size_t index, GTEXT_CSSOM_Outcome * outcome) {
  css_core * core = pl->core;
  if (index >= pl->list->count) {
    return GTEXT_CSS_E_INDEX_SIZE;
  }
  css_rule * old = css_rule_list_at(pl->list, index);
  css_ns_env * new_env = NULL;
  if (pl->ctx.top && old->kind == GTEXT_CSSOM_RULE_NAMESPACE) {
    size_t total[4];
    css_rule_list_totals(pl->list, total);
    if (total[CSS_CLASS_LAYER_STATEMENT] + total[CSS_CLASS_OTHER] > 0) {
      return GTEXT_CSS_E_INVALID_STATE;   /* "If old rule is an @namespace at-rule, and list contains anything other than @import at-rules, and @namespace at-rules" */
    }
    const GTEXT_CSS_Status st = rebuild_namespaces(core, pl->sheet, old, &new_env);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  const size_t removed = 1 + old->descendants;
  css_obj_retain_external(&old->obj);   /* the notification is about this rule; it is freed after it */
  css_rule_list_remove(pl->list, old);  /* "Remove rule old rule from list ... Set old rule's parent CSS rule and parent CSS style sheet to null" */
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = GTEXT_CSSOM_EVENT_RULE_REMOVED;
  ev.has_position = true;   /* the rule is out of its list: its path is where it was */
  ev.position_parent = pl->list->owner->kind == GTEXT_CSSOM_OBJECT_RULE ? (const void *)pl->list->owner : NULL;
  ev.position_index = index;
  settle(pl, old, removed, false, &ev);
  css_rule_touch(old);
  if (new_env) {
    css_ns_env_release(pl->sheet->ns);
    pl->sheet->ns = new_env;
  }
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
  }
  css_obj_release_internal(&old->obj);   /* the list's */
  css_cssom_commit(core, &ev, outcome);
  css_obj_release_external(&old->obj);
  return GTEXT_CSS_OK;
}

/* ---- the public calls ----------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status sheet_open(css_sheet * sheet, GTEXT_CSSOM_Outcome * outcome) {
  css_outcome_reset(outcome, sheet->obj.core);
  GTEXT_CSS_Status st = css_cssom_enter(sheet->obj.core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!sheet->origin_clean) {
    return GTEXT_CSS_E_SECURITY;   /* "If the origin-clean flag is unset, throw a SecurityError exception." */
  }
  if (sheet->disallow_modification) {
    return GTEXT_CSS_E_NOT_ALLOWED;   /* "If the disallow modification flag is set, throw a NotAllowedError DOMException." */
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_sheet_insert_rule(GTEXT_CSSOM_Sheet * sheet, const char * text, size_t length, size_t index, GTEXT_CSSOM_Rule ** rule_out, GTEXT_CSSOM_Outcome * outcome) {
  if (!sheet || (!text && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = sheet_open(sheet, outcome);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  place pl;
  place_for_sheet(sheet, &pl);
  return insert_text(&pl, text, length, index, false, rule_out, outcome);
}

GTEXT_CSS_Status gtext_cssom_sheet_delete_rule(GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Outcome * outcome) {
  if (!sheet) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = sheet_open(sheet, outcome);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  place pl;
  place_for_sheet(sheet, &pl);
  return delete_at(&pl, index, outcome);
}

GTEXT_CSS_Status gtext_cssom_sheet_add_rule(GTEXT_CSSOM_Sheet * sheet, const char * selector, size_t selector_length, const char * style, size_t style_length, bool has_index, size_t index,
    GTEXT_CSSOM_Outcome * outcome) {
  if (!sheet || (!selector && selector_length) || (!style && style_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  /* the legacy `addRule`: the selector, " { ", the block and a space when there is one, and "}", inserted at the index or at the end */
  css_core * core = sheet->obj.core;
  GTEXT_Allocator a = core->alloc;
  if (selector_length > SIZE_MAX - style_length - 8) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  const size_t n = selector_length + 3 + (style_length ? style_length + 1 : 0) + 1;
  char * rule = gtext_allocator_malloc(&a, n);
  if (!rule) {
    css_outcome_reset(outcome, core);
    return GTEXT_CSS_E_OOM;
  }
  char * w = rule;
  if (selector_length) {
    memcpy(w, selector, selector_length);
  }
  w += selector_length;
  memcpy(w, " { ", 3);
  w += 3;
  if (style_length) {
    memcpy(w, style, style_length);
    w += style_length;
    *w++ = ' ';
  }
  *w++ = '}';
  const GTEXT_CSS_Status st = gtext_cssom_sheet_insert_rule(sheet, rule, (size_t)(w - rule), has_index ? index : sheet->rules.count, NULL, outcome);
  gtext_allocator_free(&a, rule);
  return st;
}

GTEXT_CSS_Status gtext_cssom_sheet_remove_rule(GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Outcome * outcome) {
  return gtext_cssom_sheet_delete_rule(sheet, index, outcome);
}

/// Whether the rule has a list that `insertRule` and `deleteRule` act on: a style rule, a grouping rule (a `@layer` with a block), an `@page`.
static bool is_grouping(const css_rule * rule) {
  switch ((GTEXT_CSSOM_Rule_Kind)rule->kind) {
  case GTEXT_CSSOM_RULE_STYLE:
  case GTEXT_CSSOM_RULE_MEDIA:
  case GTEXT_CSSOM_RULE_SUPPORTS:
  case GTEXT_CSSOM_RULE_CONTAINER:
  case GTEXT_CSSOM_RULE_STARTING_STYLE:
  case GTEXT_CSSOM_RULE_SCOPE:
  case GTEXT_CSSOM_RULE_WHEN:
  case GTEXT_CSSOM_RULE_ELSE:
  case GTEXT_CSSOM_RULE_PAGE:
    return true;
  case GTEXT_CSSOM_RULE_LAYER:
    return rule->has_block;
  default:
    return false;
  }
}

GTEXT_CSS_Status gtext_cssom_rule_insert_rule(GTEXT_CSSOM_Rule * rule, const char * text, size_t length, size_t index, GTEXT_CSSOM_Rule ** rule_out, GTEXT_CSSOM_Outcome * outcome) {
  if (!rule || (!text && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_outcome_reset(outcome, rule->obj.core);
  GTEXT_CSS_Status st = css_cssom_enter(rule->obj.core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!is_grouping(rule)) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  place pl;
  place_for_rule(rule, &pl);
  return insert_text(&pl, text, length, index, false, rule_out, outcome);
}

GTEXT_CSS_Status gtext_cssom_rule_delete_rule(GTEXT_CSSOM_Rule * rule, size_t index, GTEXT_CSSOM_Outcome * outcome) {
  if (!rule) {
    return GTEXT_CSS_E_INVALID;
  }
  css_outcome_reset(outcome, rule->obj.core);
  GTEXT_CSS_Status st = css_cssom_enter(rule->obj.core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!is_grouping(rule)) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  place pl;
  place_for_rule(rule, &pl);
  return delete_at(&pl, index, outcome);
}

/* ---- keyframes ---------------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_keyframes_append_rule(GTEXT_CSSOM_Rule * rule, const char * text, size_t length, GTEXT_CSSOM_Rule ** rule_out, GTEXT_CSSOM_Outcome * outcome) {
  if (!rule || (!text && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_outcome_reset(outcome, rule->obj.core);
  GTEXT_CSS_Status st = css_cssom_enter(rule->obj.core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_KEYFRAMES) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  place pl;
  place_for_rule(rule, &pl);
  pl.place = CSSOM_PARENT_GROUP;
  pl.ctx.at_in_place = true;
  if (rule_out) {
    *rule_out = NULL;
  }
  return insert_text(&pl, text, length, rule->rules.count, true, rule_out, outcome);
}

typedef struct {
  tbuf b;
} keybuf;

static GTEXT_Markup_Status keybuf_write(void * user, const char * bytes, size_t n) {
  keybuf * k = user;
  return tb_put(&k->b, bytes, n) ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_OOM;
}

/// The canonical selector of a keyframe block (`0%`, `50%, 60%`), the key `findRule` and `deleteRule` match: "The match is not sensitive to white space around the values in the
/// list", and `from` and `to` are `0%` and `100%`. `*key` is NULL when the block's selector is not valid.
static GTEXT_CSS_Status keyframe_key(css_core * core, const GTEXT_CSS_Rule * node, char ** key) {
  *key = NULL;
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = css_atrule_parse_keyframe(core->registry, node, &core->alloc, &a);
  if (st != GTEXT_CSS_OK || !a) {
    return st;
  }
  keybuf k;
  tb_init(&k.b, &core->alloc);
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &k;
  sink.write = keybuf_write;
  st = css_atrule_serialize_part(a, false, &sink);
  gtext_css_atrule_free(a);
  if (k.b.oom) {
    st = GTEXT_CSS_E_OOM;   /* the sink that ran out of memory reported a refusal */
  }
  if (st != GTEXT_CSS_OK) {
    tb_free(&k.b);
    return st;
  }
  size_t n = 0;
  *key = tb_take(&k.b, &n);
  return *key ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

/// The last keyframe of the rule whose selector is `select` (CSS Animations 1: "the last declared CSSKeyframeRule matching the specified keyframe selector"), or NULL.
static GTEXT_CSS_Status keyframe_match(css_rule * keyframes, const char * select, size_t select_length, css_rule ** found) {
  css_core * core = keyframes->obj.core;
  GTEXT_Allocator a = core->alloc;
  *found = NULL;
  /* the selector, read as the selector of a keyframe block */
  const size_t n = select_length + 2;
  char * text = gtext_allocator_malloc(&a, n + 1);
  if (!text) {
    return GTEXT_CSS_E_OOM;
  }
  if (select_length) {
    memcpy(text, select, select_length);
  }
  memcpy(text + select_length, "{}", 3);
  place pl;
  place_for_rule(keyframes, &pl);
  GTEXT_CSS_Tree * tree = NULL;
  bool syntax = false;
  GTEXT_CSS_Status st = css_parse_inserted(core, &pl.ctx, text, n, &tree, &syntax);
  gtext_allocator_free(&a, text);
  if (st != GTEXT_CSS_OK || syntax) {
    return st;   /* an invalid selector matches nothing */
  }
  char * want = NULL;
  st = keyframe_key(core, &tree->rules[0], &want);
  gtext_css_tree_free(tree);
  if (st != GTEXT_CSS_OK || !want) {
    return st;
  }
  size_t work = 0;
  for (css_rule * k = css_rule_list_first(&keyframes->rules); k && st == GTEXT_CSS_OK; k = css_rule_list_next(k)) {
    if (++work > core->config.max_work) {
      st = GTEXT_CSS_E_LIMIT_WORK;
      break;
    }
    char * have = NULL;
    st = keyframe_key(core, k->node, &have);
    if (st == GTEXT_CSS_OK && have && strcmp(have, want) == 0) {
      *found = k;
    }
    gtext_allocator_free(&a, have);
  }
  gtext_allocator_free(&a, want);
  return st;
}

GTEXT_CSS_Status gtext_cssom_keyframes_find_rule(GTEXT_CSSOM_Rule * rule, const char * select, size_t select_length, GTEXT_CSSOM_Rule ** out) {
  if (!rule || !out || (!select && select_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_KEYFRAMES) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  css_rule * found = NULL;
  const GTEXT_CSS_Status st = keyframe_match(rule, select, select_length, &found);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *out = found ? gtext_cssom_rule_retain(found) : NULL;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_keyframes_delete_rule(GTEXT_CSSOM_Rule * rule, const char * select, size_t select_length, GTEXT_CSSOM_Outcome * outcome) {
  if (!rule || (!select && select_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_outcome_reset(outcome, rule->obj.core);
  GTEXT_CSS_Status st = css_cssom_enter(rule->obj.core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_KEYFRAMES) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  css_rule * found = NULL;
  st = keyframe_match(rule, select, select_length, &found);
  if (st != GTEXT_CSS_OK || !found) {
    return st;   /* "If no matching rule exists, the method does nothing." */
  }
  place pl;
  place_for_rule(rule, &pl);
  return delete_at(&pl, css_rule_list_index_of(found), outcome);
}
