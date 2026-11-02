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
 * The bridge from a live sheet to the compiled one (css_cssom_sheet.h): `gtext_cssom_sheet_snapshot()`, the composite version it is cached on, and the `@import` rule's hold on the
 * sheet it loaded.
 *
 * **What is compiled.** The compile of css_sheet.h over an array of sources: for each top-level rule of the live sheet, the syntax tree and node it was read from when it has not been
 * changed since (`mod` is false: no mutation touched it or a rule inside it, and no compaction left its node without the rules inside) **and its node is what its serialization reads
 * back as** (checked once per rule, see same_shape()), and otherwise the tree of the parse of its own `cssText`, read in the place it is in. So the compile sees exactly the state the
 * object model shows, which is the property `snapshot(live) == compile(serialize(live))`, and a rule whose text is already what the object model writes keeps its spans. The trees of
 * the reparsed rules live for the call.
 *
 * **Imports.** An `@import` rule holds the sheet it loaded with an owning edge, and the compile's import callback answers with that sheet's snapshot (one level down, to the depth the
 * sheet option allows). The callback is asked in the order of the imports, which is the order of the sources, so a cursor over the sources finds the live rule of the node it is given in
 * O(1) amortized; the graph is a tree (an import has one owner and refuses a cycle), so the walk visits each sheet once.
 *
 * **The cache.** A change of a sheet gives it a stamp from a counter of the document that only rises (css_sheet_bump()), so the greatest stamp among a sheet and the sheets it imports
 * is a version of the whole, which changes exactly when something in it does. The snapshot is kept with the stamp it was made at.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_doc_internal.h"
#include "cssom_rule_internal.h"
#include "cssom_typed.h"

#define CANON_UNKNOWN 0u
#define CANON_YES 1u
#define CANON_NO 2u

#define DEF_IMPORT_DEPTH ((size_t)16)
#define MAX_IMPORT_DEPTH ((size_t)64)

/* ---- the composite version -------------------------------------------------------------------------------------------------------------------- */

static size_t depth_of(const css_sheet * s) {
  return s->max_import_depth ? (s->max_import_depth > MAX_IMPORT_DEPTH ? MAX_IMPORT_DEPTH : s->max_import_depth) : DEF_IMPORT_DEPTH;
}

/// The greatest stamp among `sheet` and the sheets its imports hold, `levels` more levels down at most.
static GTEXT_CSS_Status composite(const css_sheet * sheet, size_t levels, uint64_t * out) {
  uint64_t best = sheet->stamp;
  size_t n[4];
  css_rule_list_totals(&sheet->rules, n);
  for (size_t k = 0; k < n[CSS_CLASS_IMPORT]; k++) {
    const css_rule * r = css_rule_list_nth_of_class(&sheet->rules, CSS_CLASS_IMPORT, k);
    if (!r || !r->import_sheet) {
      continue;
    }
    if (levels == 0) {
      return GTEXT_CSS_E_LIMIT_RULE_NESTING;
    }
    uint64_t sub = 0;
    const GTEXT_CSS_Status st = composite(r->import_sheet, levels - 1, &sub);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (sub > best) {
      best = sub;
    }
  }
  *out = best;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_sheet_composite_version(const GTEXT_CSSOM_Sheet * sheet, uint64_t * out) {
  if (!sheet || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  uint64_t v = 0;
  const GTEXT_CSS_Status st = composite(sheet, depth_of(sheet), &v);
  if (st == GTEXT_CSS_OK) {
    *out = v;
  }
  return st;
}

/* ---- whether a rule's own syntax is what its serialization reads back as ------------------------------------------------------------------ */

/// The declarations of a rule are what the rule object's block makes of them when it serializes (a shorthand and a longhand of it become one shorthand, a property written twice
/// once), so the compile of a rule's own node and the compile of its serialization can differ in the declarations they hold while the cascade sees the same. The snapshot keeps the
/// property that its dump is that of the compile of the serialization, so a rule may be read from its own node (and keep its spans) only when its node holds exactly what the text
/// the object model writes for it reads back as: the same rules in the same places, and the same declarations (name, importance, value) in each. This compares the two; a mismatch
/// anywhere is only a reason to read the rule from its text, which is always right. The declarations are compared as the compile reads them (name as written, importance, and the typed
/// value written as the dump writes it, over the declarations it keeps), not as tokens: `margin: 0` and `margin: 0px` are one declaration to the cascade.
typedef struct {
  const GTEXT_CSS_Rule * a;
  const GTEXT_CSS_Rule * b;
} rule_pair;

typedef struct {
  GTEXT_Allocator * alloc;
  const GTEXT_CSS_Registry * registry;
  rule_pair * stack;
  size_t sp, cap;
  GTEXT_CSS_Serialize_Options so;
} shape;

static bool same_text(const GTEXT_CSS_String * x, const GTEXT_CSS_String * y) {
  return x->length == y->length && (x->length == 0 || memcmp(x->data, y->data, x->length) == 0);
}

/// A run of declarations as the compile reads it: the ones it keeps (it drops what the registry does not know, what is not valid for the property and what it cannot judge), each
/// with its name as written, its importance and its typed value written as the dump writes it.
typedef struct {
  const GTEXT_CSS_Declaration * d;
  size_t n, i;
} run_iter;

static GTEXT_CSS_Status next_kept(shape * sh, run_iter * it, const GTEXT_CSS_Declaration ** decl, char ** text, size_t * length) {
  *decl = NULL;
  *text = NULL;
  *length = 0;
  while (it->i < it->n) {
    const GTEXT_CSS_Declaration * d = &it->d[it->i++];
    GTEXT_CSS_Property_Options po;
    memset(&po, 0, sizeof po);
    po.allocator = sh->alloc;
    GTEXT_CSS_Property_Value pv;
    memset(&pv, 0, sizeof pv);
    GTEXT_CSS_Status st = gtext_css_parse_property_declaration(sh->registry, d, &po, &pv);
    if (st == GTEXT_CSS_E_UNSUPPORTED || (st == GTEXT_CSS_OK && (pv.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN || pv.outcome == GTEXT_CSS_PROP_RESULT_INVALID))) {
      gtext_css_property_value_free(&pv);
      continue;
    }
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_property_value_serialize_buffer(&pv, &sh->so, text, length);
    }
    gtext_css_property_value_free(&pv);
    if (st == GTEXT_CSS_OK) {
      *decl = d;
    }
    return st;
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status same_run(shape * sh, const GTEXT_CSS_Declaration * x, size_t nx, const GTEXT_CSS_Declaration * y, size_t ny, bool * same) {
  run_iter ix = {x, nx, 0}, iy = {y, ny, 0};
  *same = true;
  for (;;) {
    const GTEXT_CSS_Declaration *dx = NULL, *dy = NULL;
    char *tx = NULL, *ty = NULL;
    size_t lx = 0, ly = 0;
    GTEXT_CSS_Status st = next_kept(sh, &ix, &dx, &tx, &lx);
    if (st == GTEXT_CSS_OK) {
      st = next_kept(sh, &iy, &dy, &ty, &ly);
    }
    if (st == GTEXT_CSS_OK) {
      if (!dx || !dy) {
        *same = !dx && !dy;
      }
      else {
        *same = same_text(&dx->name, &dy->name) && dx->important == dy->important && lx == ly && (lx == 0 || memcmp(tx, ty, lx) == 0);
      }
    }
    gtext_css_serialize_buffer_free(&sh->so, tx);
    gtext_css_serialize_buffer_free(&sh->so, ty);
    if (st != GTEXT_CSS_OK) {
      *same = false;
      return st;
    }
    if (!*same || !dx) {
      return GTEXT_CSS_OK;
    }
  }
}

static GTEXT_CSS_Status push_pair(shape * sh, const GTEXT_CSS_Rule * a, const GTEXT_CSS_Rule * b) {
  if (sh->sp == sh->cap) {
    const size_t cap = sh->cap ? sh->cap * 2 : 16;
    rule_pair * grown = gtext_allocator_realloc(sh->alloc, sh->stack, cap * sizeof *grown);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    sh->stack = grown;
    sh->cap = cap;
  }
  sh->stack[sh->sp].a = a;
  sh->stack[sh->sp].b = b;
  sh->sp++;
  return GTEXT_CSS_OK;
}

/// Whether the two nodes, and everything inside them, have the same shape and the same declarations. No recursion on the C stack.
static GTEXT_CSS_Status same_shape(css_core * core, const GTEXT_CSS_Rule * a, const GTEXT_CSS_Rule * b, bool * same) {
  shape sh;
  memset(&sh, 0, sizeof sh);
  sh.alloc = &core->alloc;
  sh.registry = core->registry;
  sh.so = gtext_css_serialize_options_default();
  sh.so.allocator = &core->alloc;
  *same = false;
  GTEXT_CSS_Status st = push_pair(&sh, a, b);
  bool ok = true;
  while (st == GTEXT_CSS_OK && ok && sh.sp) {
    const rule_pair p = sh.stack[--sh.sp];
    const GTEXT_CSS_Rule * x = p.a;
    const GTEXT_CSS_Rule * y = p.b;
    if (x->kind != y->kind || !same_text(&x->name, &y->name) || x->has_block != y->has_block || x->rule_count != y->rule_count || x->content_count != y->content_count) {
      ok = false;
      break;
    }
    st = same_run(&sh, x->declarations, x->declaration_count, y->declarations, y->declaration_count, &ok);
    for (size_t i = 0; st == GTEXT_CSS_OK && ok && i < x->rule_count; i++) {
      st = push_pair(&sh, &x->rules[i], &y->rules[i]);
    }
    for (size_t i = 0; st == GTEXT_CSS_OK && ok && i < x->content_count; i++) {
      const GTEXT_CSS_Block_Item * ix = &x->contents[i];
      const GTEXT_CSS_Block_Item * iy = &y->contents[i];
      if (ix->kind != iy->kind) {
        ok = false;
      }
      else if (ix->kind == GTEXT_CSS_ITEM_RULE) {
        st = push_pair(&sh, ix->rule, iy->rule);
      }
      else {
        st = same_run(&sh, ix->declarations, ix->declaration_count, iy->declarations, iy->declaration_count, &ok);
      }
    }
  }
  gtext_allocator_free(&core->alloc, sh.stack);
  *same = st == GTEXT_CSS_OK && ok;
  return st;
}

/* ---- the sources ----------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  css_core * core;
  GTEXT_CSS_Rule_Source * sources;
  css_rule ** live;               ///< By source: the top-level rule it is of.
  size_t count;
  GTEXT_CSS_Tree ** trees;        ///< The trees of the reparsed rules, freed with the call.
  size_t ntrees;
  size_t cursor;                  ///< The import callback's place in `sources`.
  size_t levels;                  ///< Levels of imports below the sheet being compiled that are allowed.
  GTEXT_CSS_Status error;         ///< The first failure of the import callback, which can only answer a sheet or none.
} snap_ctx;

static void ctx_free(snap_ctx * c) {
  GTEXT_Allocator a = c->core->alloc;
  for (size_t i = 0; i < c->ntrees; i++) {
    gtext_css_tree_free(c->trees[i]);
  }
  gtext_allocator_free(&a, c->trees);
  gtext_allocator_free(&a, c->sources);
  gtext_allocator_free(&a, c->live);
  memset(c, 0, sizeof *c);
}

/// The tree of the parse of the rule's own `cssText`, read in the place the rule is in.
static GTEXT_CSS_Status reparse(snap_ctx * c, css_rule * r, GTEXT_CSS_Rule_Source * out) {
  css_core * core = c->core;
  char * text = NULL;
  size_t length = 0;
  GTEXT_CSS_Status st = gtext_cssom_rule_css_text(r, &text, &length);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_insert_ctx ctx;
  GTEXT_CSSOM_Rule_Kind parent_kind;
  GTEXT_CSS_Rule synthetic;
  css_rule_own_ctx(r, &ctx, &parent_kind, &synthetic);
  GTEXT_CSS_Tree * tree = NULL;
  bool syntax = false;
  st = css_parse_inserted(core, &ctx, text, length, &tree, &syntax);
  gtext_cssom_buffer_free(&core->alloc, text);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (syntax) {
    return GTEXT_CSS_E_INTERNAL;   /* the serialization of a rule is a fixed point of the parse; not reaching it is a defect */
  }
  c->trees[c->ntrees++] = tree;
  out->tree = tree;
  out->rule = &tree->rules[0];
  out->source_id = 0;   /* the bytes it indexes are gone with the call */
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status build_sources(snap_ctx * c, css_sheet * sheet) {
  css_core * core = c->core;
  GTEXT_Allocator a = core->alloc;
  const size_t n = sheet->rules.count;
  if (n == 0) {
    return GTEXT_CSS_OK;
  }
  c->sources = gtext_allocator_calloc(&a, n, sizeof *c->sources);
  c->live = gtext_allocator_calloc(&a, n, sizeof *c->live);
  c->trees = gtext_allocator_calloc(&a, n, sizeof *c->trees);
  if (!c->sources || !c->live || !c->trees) {
    return GTEXT_CSS_E_OOM;
  }
  size_t i = 0;
  for (css_rule * r = css_rule_list_first(&sheet->rules); r; r = css_rule_list_next(r), i++) {
    if (i >= n) {
      return GTEXT_CSS_E_INTERNAL;
    }
    c->live[i] = r;
    c->count = i + 1;
    const bool own = !r->mod && r->node && r->source && r->source->tree && r->canon != CANON_NO;
    if (own && r->canon == CANON_UNKNOWN) {
      /* once, for a rule that has not been changed: is its own syntax what its serialization reads back as? */
      GTEXT_CSS_Status st = reparse(c, r, &c->sources[i]);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      bool same = false;
      st = same_shape(core, r->node, c->sources[i].rule, &same);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      core->counters.snapshot_rules_checked++;
      r->canon = same ? CANON_YES : CANON_NO;
      if (!same) {
        core->counters.snapshot_rules_reparsed++;
        continue;   /* the tree of the text is the source */
      }
      /* the node it is: the tree of the text is needed no more, and is freed with the call */
    }
    if (own) {
      c->sources[i].tree = r->source->tree;
      c->sources[i].rule = r->node;
      c->sources[i].source_id = r->source->id;
      core->counters.snapshot_rules_reused++;
    }
    else {
      const GTEXT_CSS_Status st = reparse(c, r, &c->sources[i]);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      core->counters.snapshot_rules_reparsed++;
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- the compile ---------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status snapshot(css_sheet * sheet, size_t levels, GTEXT_CSS_Sheet ** out);

/// The compile asks for the sheet of each `@import`, in order; the answer is the snapshot of the sheet the live import rule holds.
static GTEXT_CSS_Sheet * import_thunk(void * user, const GTEXT_CSS_Import_Request * request) {
  snap_ctx * c = user;
  if (c->error != GTEXT_CSS_OK) {
    return NULL;
  }
  while (c->cursor < c->count && c->sources[c->cursor].rule != request->rule) {
    c->cursor++;
  }
  if (c->cursor >= c->count) {
    c->error = GTEXT_CSS_E_INTERNAL;   /* an import the compile read that is not among the sources it was given */
    return NULL;
  }
  css_sheet * child = c->live[c->cursor]->import_sheet;
  c->cursor++;
  if (!child) {
    return NULL;
  }
  if (c->levels == 0) {
    c->error = GTEXT_CSS_E_INTERNAL;   /* composite() refused a sheet with an import below the last level before the compile began; the graph does not change during a call */
    return NULL;
  }
  GTEXT_CSS_Sheet * got = NULL;
  const GTEXT_CSS_Status st = snapshot(child, c->levels - 1, &got);
  if (st != GTEXT_CSS_OK) {
    c->error = st;
    return NULL;
  }
  return got;
}

/// The snapshot of the sheet and a reference for the caller; compiled when the cache is not current.
static GTEXT_CSS_Status snapshot(css_sheet * sheet, size_t levels, GTEXT_CSS_Sheet ** out) {
  css_core * core = sheet->obj.core;
  uint64_t stamp = 0;
  GTEXT_CSS_Status st = composite(sheet, levels, &stamp);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (sheet->snap && sheet->snap_stamp == stamp) {
    core->counters.snapshot_hits++;
    *out = gtext_css_sheet_retain(sheet->snap);
    return GTEXT_CSS_OK;
  }
  snap_ctx c;
  memset(&c, 0, sizeof c);
  c.core = core;
  c.levels = levels;
  st = build_sources(&c, sheet);
  if (st != GTEXT_CSS_OK) {
    ctx_free(&c);
    return st;
  }
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.allocator = &core->alloc;
  o.import = import_thunk;
  o.import_user = &c;
  o.max_rule_depth = core->config.max_rule_nesting;
  o.max_diagnostics = core->config.max_diagnostics;
  o.max_import_depth = levels ? levels : 1;   /* the compile counts the depth of the sheets below this one against it (0 would be its default; at 0 the callback has refused already) */
  GTEXT_CSS_Sheet * made = NULL;
  GTEXT_CSS_Sheet_Result result;
  st = gtext_css_sheet_compile_rules(c.sources, c.count, core->registry, &o, &made, &result);
  if (st == GTEXT_CSS_OK && c.error != GTEXT_CSS_OK) {
    gtext_css_sheet_free(made);
    st = c.error;
  }
  ctx_free(&c);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  core->counters.snapshot_compiles++;
  GTEXT_CSS_Sheet * old = sheet->snap;
  sheet->snap = made;
  sheet->snap_stamp = stamp;
  gtext_css_sheet_free(old);
  *out = gtext_css_sheet_retain(made);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_sheet_snapshot(GTEXT_CSSOM_Sheet * sheet, GTEXT_CSS_Sheet ** out) {
  if (!sheet || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Sheet * got = NULL;
  const GTEXT_CSS_Status st = snapshot(sheet, depth_of(sheet), &got);
  if (st == GTEXT_CSS_OK) {
    *out = got;
  }
  return st;
}

/* ---- imports ------------------------------------------------------------------------------------------------------------------------------------ */

GTEXT_CSS_Status gtext_cssom_sheet_owner_rule(const GTEXT_CSSOM_Sheet * sheet, GTEXT_CSSOM_Rule ** out) {
  if (!sheet || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = gtext_cssom_rule_retain(sheet->owner_rule);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_import_rule_sheet(const GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Sheet ** out) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_IMPORT) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  *out = gtext_cssom_sheet_retain(rule->import_sheet);
  return GTEXT_CSS_OK;
}

/// Whether `candidate` is `host` or holds it, through the imports above it.
static bool reaches(const css_sheet * candidate, const css_sheet * host) {
  for (const css_sheet * x = host; x; x = x->owner_rule ? css_rule_sheet(x->owner_rule) : NULL) {
    if (x == candidate) {
      return true;
    }
  }
  return false;
}

GTEXT_CSS_Status css_import_check(const css_sheet * host, const css_sheet * candidate) {
  if (host && candidate->obj.core != host->obj.core) {
    return GTEXT_CSS_E_INVALID;
  }
  if (candidate->owner_rule || candidate->owner_node || candidate->constructed || candidate->scope) {
    return GTEXT_CSS_E_INVALID_STATE;   /* single owner: the sheet has one already */
  }
  if (reaches(candidate, host)) {
    return GTEXT_CSS_E_HIERARCHY;   /* CSS Cascade 5's cycle, applied to the live graph */
  }
  return GTEXT_CSS_OK;
}

void css_import_attach(css_rule * rule, css_sheet * sheet) {
  css_obj_retain_internal(&sheet->obj);
  sheet->owner_rule = rule;
  css_obj_set_parent(&sheet->obj, &rule->obj);
  rule->import_sheet = sheet;
  rule->import_pending = false;
}

bool css_sheet_diag_reserve(css_sheet * sheet, size_t more) {
  css_core * core = sheet->obj.core;
  if (sheet->ndiags + more > core->config.max_diagnostics) {
    more = core->config.max_diagnostics > sheet->ndiags ? core->config.max_diagnostics - sheet->ndiags : 0;
  }
  if (sheet->ndiags + more <= sheet->capdiags) {
    return true;
  }
  size_t cap = sheet->capdiags ? sheet->capdiags : 16;
  while (cap < sheet->ndiags + more) {
    cap *= 2;
  }
  css_parse_diag * grown = gtext_allocator_realloc(&core->alloc, sheet->diags, cap * sizeof *grown);
  if (!grown) {
    return false;
  }
  sheet->diags = grown;
  sheet->capdiags = cap;
  return true;
}

void css_sheet_diag_push(css_sheet * sheet, GTEXT_CSSOM_Parse_Diag_Kind kind, GTEXT_CSS_Span span, const char * reference) {
  sheet->diag_total++;
  if (sheet->ndiags >= sheet->obj.core->config.max_diagnostics || sheet->ndiags >= sheet->capdiags) {
    return;
  }
  sheet->diags[sheet->ndiags].kind = kind;
  sheet->diags[sheet->ndiags].span = span;
  sheet->diags[sheet->ndiags].reference = reference;
  sheet->ndiags++;
}

#define IMPORT_NESTING_LIMIT 64u

/// A text of the rule's attribute, or NULL (and `*is_null` true) when it has none.
static GTEXT_CSS_Status import_attribute(css_rule * rule, const char * name, char ** out, size_t * length, bool * is_null) {
  *out = NULL;
  *length = 0;
  *is_null = true;
  return gtext_cssom_rule_get_attribute(rule, name, strlen(name), out, length, is_null);
}

GTEXT_CSS_Status css_import_ask(css_sheet * sheet, css_rule * rule, bool * asked, css_sheet ** made, bool * pending, bool * nesting_refused) {
  css_core * core = sheet->obj.core;
  *asked = false;
  *made = NULL;
  *pending = false;
  *nesting_refused = false;
  if (!core->config.import) {
    return GTEXT_CSS_OK;
  }
  *asked = true;
  if (core->import_depth >= IMPORT_NESTING_LIMIT) {
    core->counters.import_refused++;
    *nesting_refused = true;
    return GTEXT_CSS_OK;
  }
  char * href = NULL;
  char * layer = NULL;
  char * supports = NULL;
  char * media = NULL;
  size_t href_n = 0, layer_n = 0, supports_n = 0, media_n = 0;
  bool href_null = true, layer_null = true, supports_null = true;
  GTEXT_CSS_Status st = import_attribute(rule, "href", &href, &href_n, &href_null);
  if (st == GTEXT_CSS_OK) {
    st = import_attribute(rule, "layerName", &layer, &layer_n, &layer_null);
  }
  if (st == GTEXT_CSS_OK) {
    st = import_attribute(rule, "supportsText", &supports, &supports_n, &supports_null);
  }
  if (st == GTEXT_CSS_OK) {
    st = css_rule_media_text(rule, &media, &media_n);   /* without the MediaList object, which would take an id the callback has no use for */
  }
  if (st == GTEXT_CSS_OK) {
    GTEXT_CSSOM_Import_Request req;
    memset(&req, 0, sizeof req);
    req.size = sizeof req;
    req.rule = rule;
    req.sheet = sheet;
    req.href = href ? href : "";
    req.href_length = href_n;
    req.base_url = sheet->base_url;
    req.media = media ? media : "";
    req.supports = supports_null ? NULL : supports;
    req.layer = layer_null ? NULL : (layer ? layer : "");
    req.depth = core->import_depth;
    GTEXT_CSSOM_Import_Outcome outcome = GTEXT_CSSOM_IMPORT_FAILED;
    css_sheet * got = NULL;
    core->import_depth++;
    core->in_callback++;
    core->counters.import_requests++;
    st = core->config.import(core->config.import_user, &req, &got, &outcome);
    core->in_callback--;
    core->import_depth--;
    if (st != GTEXT_CSS_OK) {
      gtext_cssom_sheet_release(got);
    }
    else if (got) {
      *made = got;
    }
    else {
      *pending = outcome == GTEXT_CSSOM_IMPORT_PENDING;
    }
  }
  gtext_cssom_buffer_free(&core->alloc, href);
  gtext_cssom_buffer_free(&core->alloc, layer);
  gtext_cssom_buffer_free(&core->alloc, supports);
  gtext_cssom_buffer_free(&core->alloc, media);
  return st;
}

GTEXT_CSS_Status css_sheet_ask_imports(css_sheet * sheet) {
  css_core * core = sheet->obj.core;
  if (!core->config.import) {
    return GTEXT_CSS_OK;
  }
  size_t n[4];
  css_rule_list_totals(&sheet->rules, n);
  for (size_t k = 0; k < n[CSS_CLASS_IMPORT]; k++) {
    css_rule * rule = css_rule_list_nth_of_class(&sheet->rules, CSS_CLASS_IMPORT, k);
    if (!rule || rule->kind != GTEXT_CSSOM_RULE_IMPORT || rule->import_sheet || rule->import_pending) {
      continue;
    }
    bool asked = false, pending = false, nesting = false;
    css_sheet * made = NULL;
    GTEXT_CSS_Status st = css_import_ask(sheet, rule, &asked, &made, &pending, &nesting);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    GTEXT_CSS_Status chk = GTEXT_CSS_OK;
    if (made) {
      chk = css_import_check(sheet, made);
    }
    if (nesting || (made && chk != GTEXT_CSS_OK)) {
      if (!css_sheet_diag_reserve(sheet, 1)) {
        gtext_cssom_sheet_release(made);
        return GTEXT_CSS_E_OOM;
      }
      css_sheet_diag_push(sheet, GTEXT_CSSOM_PARSE_IMPORT_REFUSED, css_rule_span(rule),
          nesting ? "the import callbacks are nested deeper than the library follows"
                  : "CSS Cascade 5, @import: the sheet of an import has one owner and cannot hold the sheet that holds it");
      if (made) {
        core->counters.import_refused++;
      }
    }
    else if (made) {
      css_import_attach(rule, made);
    }
    else if (pending) {
      rule->import_pending = true;
    }
    gtext_cssom_sheet_release(made);
  }
  return GTEXT_CSS_OK;
}

size_t gtext_cssom_sheet_pending_imports(const GTEXT_CSSOM_Sheet * sheet) {
  if (!sheet) {
    return 0;
  }
  size_t n[4];
  css_rule_list_totals(&sheet->rules, n);
  size_t pending = 0;
  for (size_t k = 0; k < n[CSS_CLASS_IMPORT]; k++) {
    const css_rule * r = css_rule_list_nth_of_class(&sheet->rules, CSS_CLASS_IMPORT, k);
    pending += r && r->import_pending;
  }
  return pending;
}

bool gtext_cssom_import_rule_pending(const GTEXT_CSSOM_Rule * rule) {
  return rule && rule->kind == GTEXT_CSSOM_RULE_IMPORT && rule->import_pending;
}

GTEXT_CSS_Status gtext_cssom_import_rule_complete(GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Sheet * sheet, GTEXT_CSSOM_Outcome * outcome) {
  if (!rule) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = rule->obj.core;
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  css_outcome_reset(out, core);
  if (rule->kind != GTEXT_CSSOM_RULE_IMPORT) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_sheet * host = css_rule_sheet(rule);
  if (sheet) {
    if (sheet->obj.core != core) {
      return GTEXT_CSS_E_INVALID;
    }
    if (sheet == rule->import_sheet) {
      return GTEXT_CSS_OK;   /* UNCHANGED */
    }
    st = css_import_check(host, sheet);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  else if (!rule->import_sheet && !rule->import_pending) {
    return GTEXT_CSS_OK;   /* UNCHANGED */
  }
  /* nothing below can fail: a pending import is answered (a sheet or none) and is a change either way */
  css_sheet * old = rule->import_sheet;
  if (sheet) {
    css_import_attach(rule, sheet);
  }
  else {
    rule->import_sheet = NULL;
    rule->import_pending = false;
  }
  if (old) {
    old->owner_rule = NULL;
    css_obj_set_parent(&old->obj, NULL);
  }
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = GTEXT_CSSOM_EVENT_IMPORT_COMPLETED;
  ev.rule = rule;
  ev.rule_id = rule->obj.id;
  if (host) {
    css_sheet_bump(host);
    ev.sheet = host;
    ev.sheet_id = host->obj.id;
  }
  else {
    ev.silent = true;   /* a rule that no sheet holds: nothing that evaluates the document can see it */
  }
  out->kind = GTEXT_CSSOM_CHANGED;
  css_obj_retain_external(&rule->obj);
  css_cssom_commit(core, &ev, out);
  css_obj_release_external(&rule->obj);
  if (old) {
    css_obj_release_internal(&old->obj);
  }
  return GTEXT_CSS_OK;
}
