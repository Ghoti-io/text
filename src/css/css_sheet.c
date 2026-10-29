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
 * Compiling a parsed style sheet (css_sheet.h).
 *
 * One iterative walk of the rules (the stack is an array of frames, bounded by `max_rule_depth`; nothing recurses). A style rule's selector is parsed once; a selector that is
 * relative (nested in a style rule, or directly in an `@scope`) is given the implied `&` and a descendant combinator by rewriting its component values before the parse, so that
 * the compiled form is the ordinary one and the matcher evaluates `&` as the parent rule's selector list or as `:scope`. The layers come from the layer walker of css_layer.c, run on
 * each top-level rule on its own so that the order of the declarations and of the imports is kept. Declarations are parsed once, through the registry.
 */

#include <string.h>

#include "css_registry_internal.h"
#include "css_sheet_internal.h"

#define DEF_RULE_DEPTH ((size_t)64)
#define MAX_RULE_DEPTH ((size_t)4096)
#define DEF_BYTES ((size_t)256 * 1024 * 1024)
#define DEF_DIAGS ((size_t)1024)
#define DEF_IMPORT_DEPTH ((size_t)16)
#define MAX_IMPORT_DEPTH ((size_t)64)
#define MAX_SCOPE_DEPTH ((size_t)8)
#define DEF_CONDITIONS ((size_t)1048576)

GTEXT_CSS_Sheet_Options gtext_css_sheet_options_default(void) {
  GTEXT_CSS_Sheet_Options o;
  memset(&o, 0, sizeof o);
  o.selector = gtext_css_selector_options_default();
  o.max_rule_depth = DEF_RULE_DEPTH;
  o.max_compiled_bytes = DEF_BYTES;
  o.max_diagnostics = DEF_DIAGS;
  o.max_import_depth = DEF_IMPORT_DEPTH;
  o.max_conditions = DEF_CONDITIONS;
  o.condition = gtext_css_media_options_default();
  return o;
}

/* ---- the sheet -------------------------------------------------------------------------------------------- */

static void sheet_destroy(GTEXT_CSS_Sheet * s) {
  if (!s) {
    return;
  }
  for (size_t i = 0; i < s->nruns; i++) {
    for (size_t j = 0; j < s->runs[i].ndecl; j++) {
      gtext_css_property_value_free(&s->runs[i].decls[j].pv);
    }
  }
  for (size_t i = 0; i < s->nlists; i++) {
    gtext_css_selector_list_free(s->lists[i]);
  }
  for (size_t i = 0; i < s->nimports; i++) {
    gtext_css_sheet_free(s->imports[i]);
  }
  for (size_t i = 0; i < s->nconds; i++) {
    gtext_css_media_list_free(s->conds[i].list);
    gtext_css_media_list_free(s->conds[i].list2);
  }
  gtext_css_layer_registry_free(s->layers);
  const GTEXT_Allocator a = s->alloc;
  gtext_allocator_free(&a, s->runs);
  gtext_allocator_free(&a, s->recs);
  gtext_allocator_free(&a, (void *)s->lists);
  gtext_allocator_free(&a, s->events);
  gtext_allocator_free(&a, s->imports);
  gtext_allocator_free(&a, s->import_cond);
  gtext_allocator_free(&a, (void *)s->import_scope);
  gtext_allocator_free(&a, s->conds);
  gtext_allocator_free(&a, s->diags);
  gtext_allocator_free(&a, s->ns);
  css_arena_free(&s->arena);
  gtext_allocator_free(&a, s);
}

GTEXT_CSS_Sheet * gtext_css_sheet_retain(GTEXT_CSS_Sheet * sheet) {
  if (sheet) {
    (void)gcu_atomic_int_fetch_add(&sheet->refs, 1);
  }
  return sheet;
}

void gtext_css_sheet_free(GTEXT_CSS_Sheet * sheet) {
  if (!sheet) {
    return;
  }
  if (gcu_atomic_int_fetch_sub(&sheet->refs, 1) == 1) {
    sheet_destroy(sheet);
  }
}

static bool grow(const GTEXT_Allocator * a, void ** p, size_t * cap, size_t need, size_t elem) {
  if (need <= *cap) {
    return true;
  }
  size_t n = *cap ? *cap * 2 : 8;
  while (n < need) {
    n *= 2;
  }
  if (n > ((size_t)-1) / elem) {
    return false;
  }
  void * q = gtext_allocator_realloc(a, *p, n * elem);
  if (!q) {
    return false;
  }
  *p = q;
  *cap = n;
  return true;
}

/* ---- the build -------------------------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Rule * rules;
  size_t nrules;
  const GTEXT_CSS_Block_Item * items;
  size_t nitems;
  size_t pos;
  uint32_t layer;
  const css_nest * nest;          ///< What `&` stands for in a selector read here.
  GTEXT_CSS_Specificity nspec;    ///< Its specificity.
  const sheet_scope * scope;
  bool relative;                  ///< A selector read here gets the implied `&`.
  int decl_ctx;                   ///< 0: declarations here are nothing; 1: they are the parent style rule's (`plist`); 2: they are `&` of an `@scope`.
  const GTEXT_CSS_Selector_List * plist;
  const css_nest * pnest;
  GTEXT_CSS_Specificity pspec;
  uint32_t cond;                  ///< The condition node the rules read here are under.
  uint32_t chain;                 ///< The condition node of the `@when` or `@else` rule just before, if the last thing read here was one (the chain an `@else` joins).
} frame;

typedef struct {
  GTEXT_CSS_Sheet * sheet;
  const GTEXT_CSS_Sheet_Options * opt;
  GTEXT_CSS_Sheet_Result * res;
  const GTEXT_CSS_Tree * tree;
  frame * stack;
  size_t sp, max_depth;
  uint32_t * layer_of;   ///< By rule id: the layer an `@layer` block's contents or a layered import is in.
  uint32_t * decl_first; ///< By rule id: where its layer declarations begin in `decl_layers`.
  uint32_t * decl_count; ///< ... and how many.
  uint32_t * decl_layers; ///< The registry ids of the layers the rules declared (or found again), in order: the layer occurrences.
  size_t ndecl, capdecl;
  uint32_t chain_out;    ///< Set by visit(): the node of the `@when` / `@else` it read (else 0).
  const GTEXT_CSS_Selector_List * amp; ///< `&` alone, for the declarations directly in an `@scope`.
  size_t layer_count;
  GTEXT_CSS_Status status;
  size_t max_diag;
} build;

static GTEXT_CSS_Status bfail(build * b, GTEXT_CSS_Status st, const GTEXT_CSS_Span * span, const char * name, const char * detail) {
  if (b->status == GTEXT_CSS_OK) {
    b->status = st;
  }
  if (b->res) {
    b->res->status = st;
    b->res->message = gtext_css_status_string(st);
    if (span) {
      b->res->span = *span;
    }
    if (name) {
      css_copy_name(b->res->name, sizeof b->res->name, name);
    }
    b->res->detail = detail;
  }
  return st;
}

static bool diag(build * b, GTEXT_CSS_Sheet_Diag_Kind kind, GTEXT_CSS_Span span, GTEXT_CSS_Id id, const char * reference) {
  GTEXT_CSS_Sheet * s = b->sheet;
  s->diag_total++;
  if (s->ndiags >= b->max_diag) {
    return true;
  }
  if (!grow(&s->alloc, (void **)&s->diags, &s->capdiags, s->ndiags + 1, sizeof *s->diags)) {
    bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
    return false;
  }
  GTEXT_CSS_Sheet_Diagnostic * d = &s->diags[s->ndiags++];
  d->kind = kind;
  d->span = span;
  d->id = id;
  d->reference = reference;
  return true;
}

static bool account(build * b, size_t bytes, const GTEXT_CSS_Span * span) {
  GTEXT_CSS_Sheet * s = b->sheet;
  const size_t cap = b->opt->max_compiled_bytes ? b->opt->max_compiled_bytes : DEF_BYTES;
  if (bytes > cap || s->bytes > cap - bytes) {
    bfail(b, GTEXT_CSS_E_LIMIT_SHEET_SIZE, span, "max_compiled_bytes", "the compiled sheet would be larger than max_compiled_bytes");
    return false;
  }
  s->bytes += bytes;
  return true;
}

/// The layer walker is asked about every conditional rule and says yes to each: the conditions are data, and what is declared where is decided when the index is built.
static bool always_active(void * user, const GTEXT_CSS_Rule * rule) {
  (void)user;
  (void)rule;
  return true;
}

/// One layer occurrence: the walker's note that a rule declared (or found again) a layer.
static void declare_thunk(void * user, const GTEXT_CSS_Rule * rule, GTEXT_CSS_Layer_Id layer) {
  build * b = (build *)user;
  if (b->status != GTEXT_CSS_OK) {
    return;
  }
  if (!grow(&b->sheet->alloc, (void **)&b->decl_layers, &b->capdecl, b->ndecl + 1, sizeof *b->decl_layers)) {
    bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
    return;
  }
  if (b->decl_count[rule->id] == 0) {
    b->decl_first[rule->id] = (uint32_t)b->ndecl;
  }
  b->decl_count[rule->id]++;
  b->decl_layers[b->ndecl++] = (uint32_t)layer;
}

static void layer_thunk(void * user, const GTEXT_CSS_Rule * rule, GTEXT_CSS_Layer_Id layer) {
  build * b = (build *)user;
  b->layer_of[rule->id] = (uint32_t)layer;
}

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_delim(const GTEXT_CSS_Component_Value * v, uint32_t c) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == c;
}

/* ---- selectors ----------------------------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Component_Value * v;
  size_t n, i;
} scan;

/// Whether the values contain the nesting selector, at any depth (an iteration).
static bool has_amp(build * b, const GTEXT_CSS_Component_Value * v, size_t n, bool also_scope, bool * oom) {
  scan * st = NULL;
  size_t cap = 0, sp = 0;
  bool found = false;
  const GTEXT_Allocator * a = &b->sheet->alloc;
  if (!grow(a, (void **)&st, &cap, 1, sizeof *st)) {
    *oom = true;
    return false;
  }
  st[sp++] = (scan){v, n, 0};
  while (sp && !found) {
    scan * f = &st[sp - 1];
    if (f->i >= f->n) {
      sp--;
      continue;
    }
    const GTEXT_CSS_Component_Value * c = &f->v[f->i++];
    if (is_delim(c, '&')) {
      found = true;
    }
    else if (also_scope && c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_COLON && f->i < f->n && f->v[f->i].kind == GTEXT_CSS_VALUE_TOKEN &&
        f->v[f->i].type == GTEXT_CSS_TOKEN_IDENT && css_ascii_ieq(&f->v[f->i].value, "scope")) {
      found = true;
    }
    else if (c->kind != GTEXT_CSS_VALUE_TOKEN && c->child_count) {
      if (!grow(a, (void **)&st, &cap, sp + 1, sizeof *st)) {
        *oom = true;
        break;
      }
      st[sp++] = (scan){c->children, c->child_count, 0};
    }
  }
  gtext_allocator_free(a, st);
  return found;
}

/// Parse a selector list from a rule's prelude. `relative`: a selector with a leading combinator, or without `&`, gets `& ` in front of it. A list that is not valid is `*out` NULL.
static GTEXT_CSS_Status parse_selector(build * b, const GTEXT_CSS_Component_Value * p, size_t n, bool relative, bool in_scope, GTEXT_CSS_Span span,
    GTEXT_CSS_Selector_List ** out) {
  GTEXT_CSS_Sheet * s = b->sheet;
  *out = NULL;
  GTEXT_CSS_Selector_Options so = b->opt->selector;
  so.mode = GTEXT_CSS_SELECTOR_COMPLEX_LIST;
  so.allocator = &s->alloc;
  GTEXT_CSS_Namespace_Map map;
  map.entries = s->ns;
  map.count = s->nns;
  map.default_uri = s->default_ns;
  so.namespaces = &map;
  GTEXT_CSS_Component_Value * nv = NULL;
  const GTEXT_CSS_Component_Value * vals = p;
  size_t nvals = n;
  if (relative) {
    size_t groups = 1;
    for (size_t i = 0; i < n; i++) {
      if (p[i].kind == GTEXT_CSS_VALUE_TOKEN && p[i].type == GTEXT_CSS_TOKEN_COMMA) {
        groups++;
      }
    }
    nv = gtext_allocator_calloc(&s->alloc, n + 2 * groups + 2, sizeof *nv);
    if (!nv) {
      return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
    }
    size_t k = 0, gs = 0;
    for (size_t gi = 0; gi < groups; gi++) {
      size_t ge = gs;
      while (ge < n && !(p[ge].kind == GTEXT_CSS_VALUE_TOKEN && p[ge].type == GTEXT_CSS_TOKEN_COMMA)) {
        ge++;
      }
      size_t f = gs;
      while (f < ge && is_ws(&p[f])) {
        f++;
      }
      if (f < ge) {
        const bool explicit_comb = is_delim(&p[f], '>') || is_delim(&p[f], '+') || is_delim(&p[f], '~') || (is_delim(&p[f], '|') && f + 1 < ge && is_delim(&p[f + 1], '|'));
        bool oom = false;
        const bool amp = has_amp(b, p + f, ge - f, in_scope, &oom);
        if (oom) {
          gtext_allocator_free(&s->alloc, nv);
          return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
        }
        if (explicit_comb || !amp) {
          GTEXT_CSS_Component_Value a;
          memset(&a, 0, sizeof a);
          a.kind = GTEXT_CSS_VALUE_TOKEN;
          a.type = GTEXT_CSS_TOKEN_DELIM;
          a.delim = '&';
          a.value.data = "&";
          a.value.length = 1;
          a.span = p[f].span;
          nv[k++] = a;
          GTEXT_CSS_Component_Value w;
          memset(&w, 0, sizeof w);
          w.kind = GTEXT_CSS_VALUE_TOKEN;
          w.type = GTEXT_CSS_TOKEN_WHITESPACE;
          w.span = p[f].span;
          nv[k++] = w;
          for (size_t i = f; i < ge; i++) {
            nv[k++] = p[i];
          }
        }
        else {
          for (size_t i = gs; i < ge; i++) {
            nv[k++] = p[i];
          }
        }
      }
      else {
        for (size_t i = gs; i < ge; i++) {
          nv[k++] = p[i];
        }
      }
      if (ge < n) {
        nv[k++] = p[ge]; // the comma
      }
      gs = ge + 1;
    }
    vals = nv;
    nvals = k;
  }
  GTEXT_CSS_Selector_Result sr;
  memset(&sr, 0, sizeof sr);
  GTEXT_CSS_Selector_List * list = NULL;
  const GTEXT_CSS_Status st = gtext_css_selector_parse_values(vals, nvals, &so, &list, &sr);
  gtext_allocator_free(&s->alloc, nv);
  gtext_css_selector_result_free(&sr);
  if (st != GTEXT_CSS_OK) {
    (void)span;
    return bfail(b, st, &span, NULL, NULL);
  }
  *out = list;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status keep_list(build * b, const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Span span) {
  GTEXT_CSS_Sheet * s = b->sheet;
  if (!account(b, list->node_count * 64 + 128, &span)) {
    gtext_css_selector_list_free(list);
    return b->status;
  }
  if (!grow(&s->alloc, (void **)&s->lists, &s->caplists, s->nlists + 1, sizeof *s->lists)) {
    gtext_css_selector_list_free(list);
    return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  s->lists[s->nlists++] = list;
  return GTEXT_CSS_OK;
}

/* ---- declarations -------------------------------------------------------------------------------------------- */

/// Parse the declarations of one rule into a new run; the run is dropped (and `*made` false) when none is valid.
static GTEXT_CSS_Status make_run(build * b, const GTEXT_CSS_Declaration * d, size_t n, GTEXT_CSS_Id rule_id, GTEXT_CSS_Span span, bool * made, uint32_t * run_index) {
  GTEXT_CSS_Sheet * s = b->sheet;
  *made = false;
  if (n == 0) {
    return GTEXT_CSS_OK;
  }
  if (!account(b, n * sizeof(sheet_decl) + sizeof(sheet_run), &span)) {
    return b->status;
  }
  sheet_decl * out = css_arena_alloc(&s->arena, n * sizeof *out);
  if (!out) {
    return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  memset(out, 0, n * sizeof *out);
  size_t m = 0;
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &s->alloc;
  po.quirks = b->opt->quirks;
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Property_Value pv;
    memset(&pv, 0, sizeof pv);
    const GTEXT_CSS_Status st = gtext_css_parse_property_declaration(s->registry, &d[i], &po, &pv);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      gtext_css_property_value_free(&pv);
      if (!diag(b, GTEXT_CSS_SHEET_DIAG_UNJUDGEABLE, d[i].span, d[i].id, "the value of this property cannot be judged: the declaration is dropped")) {
        goto fail;
      }
      continue;
    }
    if (st != GTEXT_CSS_OK) {
      gtext_css_property_value_free(&pv);
      bfail(b, st, &d[i].span, NULL, NULL);
      goto fail;
    }
    if (pv.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN || pv.outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
      const bool unknown = pv.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN;
      gtext_css_property_value_free(&pv);
      if (!diag(b, unknown ? GTEXT_CSS_SHEET_DIAG_UNKNOWN_PROPERTY : GTEXT_CSS_SHEET_DIAG_INVALID_VALUE, d[i].span, d[i].id,
              unknown ? "CSS Syntax 5.5.6: an unknown property is dropped" : "CSS Cascading 4: an invalid declaration is dropped")) {
        goto fail;
      }
      continue;
    }
    char * nm = css_arena_strdup(&s->arena, d[i].name.data, d[i].name.length);
    if (!nm) {
      gtext_css_property_value_free(&pv);
      bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
      goto fail;
    }
    sheet_decl * o = &out[m++];
    o->pv = pv;
    o->name = nm;
    o->name_len = d[i].name.length;
    o->id = d[i].id;
    o->span = d[i].span;
    o->important = d[i].important;
    o->custom = d[i].name.length >= 2 && d[i].name.data[0] == '-' && d[i].name.data[1] == '-';
  }
  if (m == 0) {
    return GTEXT_CSS_OK;
  }
  if (!grow(&s->alloc, (void **)&s->runs, &s->capruns, s->nruns + 1, sizeof *s->runs)) {
    bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
    goto fail;
  }
  sheet_run * r = &s->runs[s->nruns];
  r->decls = out;
  r->ndecl = m;
  r->rule_id = rule_id;
  r->span = span;
  *run_index = (uint32_t)s->nruns;
  s->nruns++;
  *made = true;
  return GTEXT_CSS_OK;
fail:
  for (size_t j = 0; j < m; j++) {
    gtext_css_property_value_free(&out[j].pv);
  }
  return b->status;
}

/// The flags of a record, from the shape of the complex selector.
static uint32_t record_flags(const GTEXT_CSS_Complex * cx) {
  uint32_t f = 0;
  const GTEXT_CSS_Compound * last = &cx->compounds[cx->compound_count - 1];
  const size_t k = last->first_pseudo_element;
  if (k < last->simple_count) {
    size_t pes = 0;
    for (size_t i = k; i < last->simple_count; i++) {
      pes += last->simples[i].kind == GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT;
    }
    const uint16_t first = last->simples[k].pseudo;
    if (first == GTEXT_CSS_SEL_PE_SLOTTED_FN) {
      f |= REC_SLOTTED | (pes > 1 ? REC_PSEUDO : 0u);
    }
    else if (first == GTEXT_CSS_SEL_PE_PART_FN) {
      f |= REC_PART | (pes > 1 ? REC_PSEUDO : 0u);
    }
    else {
      f |= REC_PSEUDO;
    }
  }
  for (size_t i = 0; i < k; i++) {
    const GTEXT_CSS_Simple * s = &last->simples[i];
    if (s->kind == GTEXT_CSS_SIMPLE_PSEUDO_CLASS &&
        (s->pseudo == GTEXT_CSS_SEL_PC_HOST || s->pseudo == GTEXT_CSS_SEL_PC_HOST_FN || s->pseudo == GTEXT_CSS_SEL_PC_HOST_CONTEXT_FN)) {
      f |= REC_HOST;
    }
  }
  return f;
}

static GTEXT_CSS_Status add_records(build * b, const frame * cur, const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Specificity base, const css_nest * nest, uint32_t run,
    bool nested, GTEXT_CSS_Span span) {
  GTEXT_CSS_Sheet * s = b->sheet;
  for (size_t i = 0; i < list->count; i++) {
    const GTEXT_CSS_Complex * cx = &list->complex[i];
    if (cx->never || cx->raw.data) {
      continue;
    }
    if (!account(b, sizeof(sheet_record), &span)) {
      return b->status;
    }
    if (!grow(&s->alloc, (void **)&s->recs, &s->caprecs, s->nrecs + 1, sizeof *s->recs)) {
      return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
    }
    sheet_record * r = &s->recs[s->nrecs++];
    r->list = list;
    r->cx = (uint32_t)i;
    r->spec = gtext_css_complex_specificity(cx, base);
    r->run = run;
    r->layer = cur->layer;
    r->nest = nest;
    r->scope = cur->scope;
    r->cond = cur->cond;
    r->flags = record_flags(cx) | (nested ? REC_NESTED : 0u);
  }
  return GTEXT_CSS_OK;
}

/// The declarations of a nested declarations rule, of a run directly in an `@scope`, or of a group rule inside a style rule: the parent's selector with a place of their own.
static GTEXT_CSS_Status declarations_of_parent(build * b, const frame * cur, const GTEXT_CSS_Declaration * d, size_t n, GTEXT_CSS_Id id, GTEXT_CSS_Span span) {
  if (cur->decl_ctx == 0 || n == 0) {
    return GTEXT_CSS_OK;
  }
  bool made = false;
  uint32_t run = 0;
  GTEXT_CSS_Status st = make_run(b, d, n, id, span, &made, &run);
  if (st != GTEXT_CSS_OK || !made) {
    return st;
  }
  if (cur->decl_ctx == 1) {
    return add_records(b, cur, cur->plist, cur->pspec, cur->pnest, run, true, span);
  }
  if (!b->amp) {
    GTEXT_CSS_Selector_Options so = b->opt->selector;
    so.mode = GTEXT_CSS_SELECTOR_COMPLEX_LIST;
    so.allocator = &b->sheet->alloc;
    GTEXT_CSS_Selector_Result sr;
    memset(&sr, 0, sizeof sr);
    GTEXT_CSS_Selector_List * l = NULL;
    st = gtext_css_selector_parse("&", 1, &so, &l, &sr);
    gtext_css_selector_result_free(&sr);
    if (st != GTEXT_CSS_OK || !l) {
      return bfail(b, st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_INTERNAL, &span, NULL, NULL);
    }
    st = keep_list(b, l, span);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    b->amp = l;
  }
  return add_records(b, cur, b->amp, gtext_css_specificity_make(0, 0, 0), NULL, run, false, span);
}

/* ---- rules ---------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status push(build * b, const frame * f, const GTEXT_CSS_Span * span) {
  if (b->sp >= b->max_depth) {
    return bfail(b, GTEXT_CSS_E_LIMIT_RULE_NESTING, span, "max_rule_depth", "rules are nested deeper than max_rule_depth");
  }
  b->stack[b->sp++] = *f;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status style_rule(build * b, const GTEXT_CSS_Rule * rule, const frame * cur) {
  GTEXT_CSS_Selector_List * list = NULL;
  GTEXT_CSS_Status st = parse_selector(b, rule->prelude, rule->prelude_count, cur->relative, cur->decl_ctx == 2, rule->span, &list);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!list) {
    return diag(b, GTEXT_CSS_SHEET_DIAG_INVALID_SELECTOR, rule->span, rule->id, "Selectors 4, 3.6: an invalid selector makes its rule invalid, with what is nested in it") ?
        GTEXT_CSS_OK :
        b->status;
  }
  st = keep_list(b, list, rule->span);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  bool made = false;
  uint32_t run = 0;
  st = make_run(b, rule->declarations, rule->declaration_count, rule->id, rule->span, &made, &run);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (made) {
    st = add_records(b, cur, list, cur->nspec, cur->nest, run, cur->decl_ctx == 1, rule->span);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  if (rule->rule_count == 0) {
    return GTEXT_CSS_OK;
  }
  css_nest * nn = css_arena_alloc(&b->sheet->arena, sizeof *nn);
  if (!nn) {
    return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  nn->list = list;
  nn->parent = cur->nest;
  frame f = *cur;
  f.rules = rule->rules;
  f.nrules = rule->rule_count;
  f.items = NULL;
  f.nitems = 0;
  f.pos = 0;
  f.nest = nn;
  f.nspec = gtext_css_selector_list_specificity(list, cur->nspec);
  f.relative = true;
  f.decl_ctx = 1;
  f.plist = list;
  f.pnest = cur->nest;
  f.pspec = cur->nspec;
  return push(b, &f, &rule->span);
}

static bool conditional_name(const GTEXT_CSS_String * n) {
  return css_ascii_ieq(n, "media") || css_ascii_ieq(n, "supports") || css_ascii_ieq(n, "container") || css_ascii_ieq(n, "starting-style") ||
      css_ascii_ieq(n, "document") || css_ascii_ieq(n, "-moz-document") || css_ascii_ieq(n, "when") || css_ascii_ieq(n, "else");
}

/// Whether the contents of a group rule hold anything the sheet would compile (a style rule, a declaration, a layer, a scope), looking through groups.
static bool holds_styles(build * b, const GTEXT_CSS_Rule * rule, bool * oom) {
  struct item {
    const GTEXT_CSS_Block_Item * it;
    size_t n, i;
  } * st = NULL;
  size_t cap = 0, sp = 0;
  bool found = false;
  if (!grow(&b->sheet->alloc, (void **)&st, &cap, 1, sizeof *st)) {
    *oom = true;
    return false;
  }
  st[sp++] = (struct item){rule->contents, rule->content_count, 0};
  while (sp && !found && !*oom) {
    struct item * f = &st[sp - 1];
    if (f->i >= f->n) {
      sp--;
      continue;
    }
    const GTEXT_CSS_Block_Item * it = &f->it[f->i++];
    if (it->kind == GTEXT_CSS_ITEM_DECLARATIONS) {
      found = it->declaration_count > 0;
      continue;
    }
    const GTEXT_CSS_Rule * r = it->rule;
    if (r->kind != GTEXT_CSS_RULE_AT) {
      found = true;
      continue;
    }
    if (css_ascii_ieq(&r->name, "layer") || css_ascii_ieq(&r->name, "scope")) {
      found = true;
    }
    else if (conditional_name(&r->name) && r->content_count) {
      if (!grow(&b->sheet->alloc, (void **)&st, &cap, sp + 1, sizeof *st)) {
        *oom = true;
        break;
      }
      st[sp++] = (struct item){r->contents, r->content_count, 0};
    }
  }
  gtext_allocator_free(&b->sheet->alloc, st);
  return found;
}

/// Some selector of the list ends in a pseudo-element. A scope's root and limit are elements, not pseudo-elements, so the prelude is invalid and the rule is dropped (the browsers do the same).
static bool list_ends_in_pseudo_element(const GTEXT_CSS_Selector_List * l) {
  for (size_t i = 0; l && i < l->count; i++) {
    const GTEXT_CSS_Complex * cx = &l->complex[i];
    for (size_t j = 0; j < cx->compound_count; j++) {
      if (cx->compounds[j].first_pseudo_element < cx->compounds[j].simple_count) {
        return true;
      }
    }
  }
  return false;
}

/// Read `(<start>)? to (<end>)?` (an `@scope` prelude, or what `scope()` of an `@import` holds in parentheses). `bad`: it is not valid. The lists are not yet kept.
static GTEXT_CSS_Status read_scope_boundaries(build * b, const GTEXT_CSS_Component_Value * p, size_t n, bool relative, bool in_scope, GTEXT_CSS_Span span,
    GTEXT_CSS_Selector_List ** start_out, GTEXT_CSS_Selector_List ** end_out, bool * bad_out) {
  size_t i = 0;
  const GTEXT_CSS_Component_Value * sv = NULL;
  size_t sn = 0;
  const GTEXT_CSS_Component_Value * ev = NULL;
  size_t en = 0;
  bool has_start = false, has_end = false, bad = false;
  *start_out = NULL;
  *end_out = NULL;
  while (i < n && is_ws(&p[i])) {
    i++;
  }
  if (i < n && p[i].kind == GTEXT_CSS_VALUE_BLOCK && p[i].type == GTEXT_CSS_TOKEN_OPEN_PAREN) {
    has_start = true;
    sv = p[i].children;
    sn = p[i].child_count;
    i++;
    while (i < n && is_ws(&p[i])) {
      i++;
    }
  }
  if (i < n) {
    if (p[i].kind == GTEXT_CSS_VALUE_TOKEN && p[i].type == GTEXT_CSS_TOKEN_IDENT && css_ascii_ieq(&p[i].value, "to")) {
      i++;
      while (i < n && is_ws(&p[i])) {
        i++;
      }
      if (i < n && p[i].kind == GTEXT_CSS_VALUE_BLOCK && p[i].type == GTEXT_CSS_TOKEN_OPEN_PAREN) {
        has_end = true;
        ev = p[i].children;
        en = p[i].child_count;
        i++;
        while (i < n && is_ws(&p[i])) {
          i++;
        }
      }
      else {
        bad = true;
      }
    }
    else {
      bad = true;
    }
  }
  if (i < n) {
    bad = true;
  }
  GTEXT_CSS_Selector_List *start = NULL, *end = NULL;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (!bad && has_start) {
    st = parse_selector(b, sv, sn, relative, in_scope, span, &start); // inside a rule or another @scope the roots are inside what that is (the implied `&`)
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    bad = start == NULL || list_ends_in_pseudo_element(start);
  }
  if (!bad && has_end) {
    st = parse_selector(b, ev, en, true, true, span, &end);
    if (st != GTEXT_CSS_OK) {
      gtext_css_selector_list_free(start);
      return st;
    }
    bad = end == NULL || list_ends_in_pseudo_element(end);
  }
  if (bad) {
    gtext_css_selector_list_free(start);
    gtext_css_selector_list_free(end);
    start = end = NULL;
  }
  *start_out = start;
  *end_out = end;
  *bad_out = bad;
  return GTEXT_CSS_OK;
}

/// Keep a scope's lists and make the scope.
static GTEXT_CSS_Status make_scope(build * b, GTEXT_CSS_Selector_List * start, GTEXT_CSS_Selector_List * end, const css_nest * outer_nest, const sheet_scope * parent,
    GTEXT_CSS_Span span, sheet_scope ** out) {
  GTEXT_CSS_Status st;
  if (start && (st = keep_list(b, start, span)) != GTEXT_CSS_OK) {
    gtext_css_selector_list_free(end);
    return st;
  }
  if (end && (st = keep_list(b, end, span)) != GTEXT_CSS_OK) {
    return st;
  }
  sheet_scope * sc = css_arena_alloc(&b->sheet->arena, sizeof *sc);
  if (!sc) {
    return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  sc->start = start;
  sc->end = end;
  sc->outer_nest = outer_nest;
  sc->parent = parent;
  sc->depth = parent ? parent->depth + 1 : 1;
  *out = sc;
  return GTEXT_CSS_OK;
}

/// `@scope (<start>)? to (<end>)?`.
static GTEXT_CSS_Status scope_rule(build * b, const GTEXT_CSS_Rule * rule, const frame * cur) {
  if (cur->scope && cur->scope->depth >= MAX_SCOPE_DEPTH) {
    return bfail(b, GTEXT_CSS_E_LIMIT_RULE_NESTING, &rule->span, "max_scope_depth", "@scope rules are nested deeper than 8");
  }
  GTEXT_CSS_Selector_List *start = NULL, *end = NULL;
  bool bad = false;
  GTEXT_CSS_Status st = read_scope_boundaries(b, rule->prelude, rule->prelude_count, cur->relative, cur->decl_ctx == 2, rule->span, &start, &end, &bad);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (bad) {
    return diag(b, GTEXT_CSS_SHEET_DIAG_INVALID_RULE, rule->span, rule->id, "CSS Cascading 6, 2.1: the prelude of @scope is not valid: the rule is dropped") ? GTEXT_CSS_OK : b->status;
  }
  sheet_scope * sc = NULL;
  st = make_scope(b, start, end, cur->nest, cur->scope, rule->span, &sc);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  frame f = *cur;
  f.rules = NULL;
  f.nrules = 0;
  f.items = rule->contents;
  f.nitems = rule->content_count;
  f.pos = 0;
  f.scope = sc;
  f.nest = NULL;
  f.nspec = gtext_css_specificity_make(0, 0, 0);
  f.relative = true;
  f.decl_ctx = 2;
  f.plist = NULL;
  f.pnest = NULL;
  f.pspec = f.nspec;
  return push(b, &f, &rule->span);
}

/* ---- condition nodes -------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status new_cond(build * b, sheet_cond_kind kind, uint32_t parent, uint32_t prev, GTEXT_CSS_Span span, uint32_t * id) {
  GTEXT_CSS_Sheet * s = b->sheet;
  const size_t cap = b->opt->max_conditions ? b->opt->max_conditions : DEF_CONDITIONS;
  if (s->nconds >= cap) {
    return bfail(b, GTEXT_CSS_E_LIMIT_CONDITIONS, &span, "max_conditions", "the sheet has more condition nodes than max_conditions");
  }
  if (!account(b, sizeof(sheet_cond), &span)) {
    return b->status;
  }
  if (!grow(&s->alloc, (void **)&s->conds, &s->capconds, s->nconds + 1, sizeof *s->conds)) {
    return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  sheet_cond * c = &s->conds[s->nconds];
  memset(c, 0, sizeof *c);
  c->kind = (uint8_t)kind;
  c->parent = parent;
  c->prev = prev;
  c->span = span;
  if (parent) {
    c->flags = (uint8_t)(s->conds[parent - 1].flags & (SCF_STARTING | SCF_ELEMENT));
  }
  if (kind == SCK_STARTING_STYLE) {
    c->flags |= SCF_STARTING;
  }
  if (kind == SCK_CONTAINER) {
    c->flags |= SCF_ELEMENT;
  }
  s->nconds++;
  *id = (uint32_t)s->nconds;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Media_Options condition_options(build * b) {
  GTEXT_CSS_Media_Options mo = b->opt->condition;
  mo.allocator = &b->sheet->alloc;
  if (!mo.parse.allocator) {
    mo.parse.allocator = &b->sheet->alloc;
  }
  return mo;
}

/// Parse the prelude of a conditional group rule into a new node under `cur`. A condition that cannot be read is a node that never applies, with a diagnostic.
static GTEXT_CSS_Status condition_of(build * b, const GTEXT_CSS_Rule * rule, const frame * cur, uint32_t * id) {
  const GTEXT_CSS_String * nm = &rule->name;
  GTEXT_CSS_Media_Options mo = condition_options(b);
  GTEXT_CSS_Media_Result mr;
  memset(&mr, 0, sizeof mr);
  GTEXT_CSS_Media_List * list = NULL;
  sheet_cond_kind kind = SCK_NEVER;
  uint8_t flags = 0;
  uint32_t prev = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const char * invalid = NULL;
  bool bare = false;
  if (css_ascii_ieq(nm, "media")) {
    kind = SCK_MEDIA;
    st = gtext_css_media_list_parse_values(rule->prelude, rule->prelude_count, &mo, &list, &mr);
  }
  else if (css_ascii_ieq(nm, "supports")) {
    kind = SCK_SUPPORTS;
    st = gtext_css_supports_parse_values(rule->prelude, rule->prelude_count, false, &mo, &list, &mr);
    invalid = "Conditional 3, 6.1: a @supports condition that does not match the grammar makes the rule never apply";
  }
  else if (css_ascii_ieq(nm, "container")) {
    kind = SCK_CONTAINER;
    st = gtext_css_container_parse_values(rule->prelude, rule->prelude_count, &mo, &list, &mr);
    invalid = "Conditional 5, 5.4: a container condition that does not match the grammar makes the rule invalid";
  }
  else if (css_ascii_ieq(nm, "when") || css_ascii_ieq(nm, "else")) {
    kind = SCK_WHEN;
    bool only_ws = true;
    for (size_t i = 0; i < rule->prelude_count; i++) {
      only_ws = only_ws && is_ws(&rule->prelude[i]);
    }
    if (css_ascii_ieq(nm, "else")) {
      flags |= SCF_ELSE;
      bare = only_ws;
    }
    if (!bare) {
      st = gtext_css_when_parse_values(rule->prelude, rule->prelude_count, &mo, &list, &mr);
      invalid = "Conditional 5, 3: a boolean condition that does not match the grammar makes the rule never apply";
    }
  }
  else if (css_ascii_ieq(nm, "starting-style")) {
    kind = SCK_STARTING_STYLE;
    for (size_t i = 0; i < rule->prelude_count; i++) {
      if (!is_ws(&rule->prelude[i])) {
        kind = SCK_NEVER;
        invalid = "Transitions 2, 3: @starting-style has no prelude";
      }
    }
  }
  else {
    invalid = "@document is not a rule any browser applies";
  }
  if (st != GTEXT_CSS_OK) {
    const GTEXT_CSS_Span failed_at = mr.failure;
    gtext_css_media_result_free(&mr);
    return bfail(b, st, &failed_at, NULL, NULL);
  }
  if (list && kind != SCK_MEDIA && mr.replaced > 0 && kind != SCK_WHEN) {
    /* a supports condition or a container query that is not valid: the rule never applies */
    gtext_css_media_list_free(list);
    list = NULL;
    kind = SCK_NEVER;
  }
  const bool report = mr.replaced > 0 || kind == SCK_NEVER;
  gtext_css_media_result_free(&mr);
  if (kind == SCK_NEVER && list) {
    gtext_css_media_list_free(list);
    list = NULL;
  }
  if (report && !diag(b, GTEXT_CSS_SHEET_DIAG_INVALID_RULE, rule->span, rule->id,
          invalid ? invalid : "Media Queries 4, 3.2: an invalid media query is replaced by `not all`")) {
    gtext_css_media_list_free(list);
    return b->status;
  }
  if (kind == SCK_WHEN) {
    /* the chain: an @else joins the @when or @else right before it; the first of a chain begins one */
    if (flags & SCF_ELSE) {
      if (!cur->chain) {
        gtext_css_media_list_free(list);
        b->chain_out = 0;
        return diag(b, GTEXT_CSS_SHEET_DIAG_INVALID_RULE, rule->span, rule->id, "Conditional 5, 4: an @else that is not part of a conditional rule chain is invalid") ? GTEXT_CSS_OK : b->status;
      }
      prev = cur->chain;
    }
  }
  GTEXT_CSS_Status ns = new_cond(b, kind, cur->cond, prev, rule->span, id);
  if (ns != GTEXT_CSS_OK) {
    gtext_css_media_list_free(list);
    return ns;
  }
  sheet_cond * c = &b->sheet->conds[*id - 1];
  c->flags |= flags;
  c->list = list;
  if (kind == SCK_WHEN) {
    b->chain_out = *id;
  }
  return GTEXT_CSS_OK;
}

/// Note the layer occurrences a rule made as events under a condition node: the index declares the layer where the node's chain holds.
static GTEXT_CSS_Status add_event(build * b, uint8_t kind, uint32_t node, uint32_t import, uint32_t cond);

static GTEXT_CSS_Status emit_declarations(build * b, const GTEXT_CSS_Rule * rule, uint32_t cond) {
  const uint32_t n = b->decl_count[rule->id];
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t no = sheet_layer_no(b->sheet, b->decl_layers[b->decl_first[rule->id] + i]);
    const GTEXT_CSS_Status st = add_event(b, 0, no, 0, cond);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  return GTEXT_CSS_OK;
}

/// One rule that is not an import, a namespace or a charset.
static GTEXT_CSS_Status visit(build * b, const GTEXT_CSS_Rule * rule, const frame * cur) {
  b->chain_out = 0;
  if (rule->kind == GTEXT_CSS_RULE_QUALIFIED) {
    return style_rule(b, rule, cur);
  }
  if (rule->kind == GTEXT_CSS_RULE_NESTED_DECLARATIONS) {
    return declarations_of_parent(b, cur, rule->declarations, rule->declaration_count, rule->id, rule->span);
  }
  if (css_ascii_ieq(&rule->name, "layer")) {
    GTEXT_CSS_Status st = emit_declarations(b, rule, cur->cond);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (!rule->has_block) {
      return GTEXT_CSS_OK;
    }
    const uint32_t l = sheet_layer_no(b->sheet, b->layer_of[rule->id]);
    if (!l || !b->layer_of[rule->id]) {
      return GTEXT_CSS_OK; // not a valid @layer block: nothing was declared
    }
    frame f = *cur;
    f.rules = NULL;
    f.nrules = 0;
    f.items = rule->contents;
    f.nitems = rule->content_count;
    f.pos = 0;
    f.layer = l;
    return push(b, &f, &rule->span);
  }
  if (css_ascii_ieq(&rule->name, "scope")) {
    if (!rule->has_block) {
      return GTEXT_CSS_OK;
    }
    return scope_rule(b, rule, cur);
  }
  if (conditional_name(&rule->name)) {
    const bool chained = css_ascii_ieq(&rule->name, "when") || css_ascii_ieq(&rule->name, "else");
    if (!rule->has_block) {
      return GTEXT_CSS_OK;
    }
    if (!chained) {
      if (rule->content_count == 0) {
        return GTEXT_CSS_OK;
      }
      bool oom = false;
      const bool holds = holds_styles(b, rule, &oom);
      if (oom) {
        return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
      }
      if (!holds) {
        return GTEXT_CSS_OK; // nothing in it is the cascade's: no node
      }
    }
    uint32_t id = 0;
    const GTEXT_CSS_Status st = condition_of(b, rule, cur, &id);
    if (st != GTEXT_CSS_OK || !id) {
      return st;
    }
    frame f = *cur;
    f.rules = NULL;
    f.nrules = 0;
    f.items = rule->contents;
    f.nitems = rule->content_count;
    f.pos = 0;
    f.cond = id;
    f.chain = 0;
    return push(b, &f, &rule->span);
  }
  return GTEXT_CSS_OK; // @font-face, @keyframes, @page, @property, ...: not the cascade's rules here
}

/* ---- layers and the top level ----------------------------------------------------------------------------------- */

static GTEXT_CSS_Status add_event(build * b, uint8_t kind, uint32_t node, uint32_t import, uint32_t cond) {
  GTEXT_CSS_Sheet * s = b->sheet;
  if (!grow(&s->alloc, (void **)&s->events, &s->capevents, s->nevents + 1, sizeof *s->events)) {
    return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  s->events[s->nevents++] = (sheet_event){kind, node, import, cond};
  return GTEXT_CSS_OK;
}

/// Declare, in the sheet's own registry, the layers one top-level rule declares (with every conditional rule taken as true: the index decides where each applies), and note which
/// rule made which layer occurrence.
static GTEXT_CSS_Status layers_of(build * b, const GTEXT_CSS_Rule * rule) {
  GTEXT_CSS_Tree t;
  memset(&t, 0, sizeof t);
  t.kind = GTEXT_CSS_TREE_STYLESHEET_CONTENTS;
  t.rules = rule;
  t.rule_count = 1;
  t.node_count = b->tree->node_count;
  GTEXT_CSS_Layer_Collect_Options lo;
  memset(&lo, 0, sizeof lo);
  lo.user = b;
  lo.active = always_active;
  lo.on_layer = layer_thunk;
  lo.on_declare = declare_thunk;
  lo.max_rule_depth = b->max_depth;
  GTEXT_CSS_Layer_Result lr;
  memset(&lr, 0, sizeof lr);
  const GTEXT_CSS_Status st = gtext_css_cascade_collect_layers(b->sheet->layers, GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_LAYER_NONE, &t, &lo, &lr);
  if (b->status != GTEXT_CSS_OK) {
    return b->status;
  }
  if (st != GTEXT_CSS_OK) {
    return bfail(b, st == GTEXT_CSS_E_LIMIT_DEPTH ? GTEXT_CSS_E_LIMIT_RULE_NESTING : st, &lr.span, lr.name[0] ? lr.name : NULL, lr.detail);
  }
  const size_t now = gtext_css_layer_count(b->sheet->layers, GTEXT_CSS_ORIGIN_AUTHOR);
  if (b->sheet->first_id == 0 && now > 0) {
    GTEXT_CSS_Layer_Info info;
    for (uint32_t id = 1; id <= 64 && b->sheet->first_id == 0; id++) {
      if (gtext_css_layer_info(b->sheet->layers, id, &info)) {
        b->sheet->first_id = id;
      }
    }
    if (b->sheet->first_id == 0) {
      return bfail(b, GTEXT_CSS_E_INTERNAL, NULL, NULL, "the first layer of the registry was not found");
    }
  }
  b->layer_count = now;
  return GTEXT_CSS_OK;
}

/// The text of component values, for a request.
static GTEXT_CSS_Status text_of(build * b, const GTEXT_CSS_Component_Value * v, size_t n, char ** out, size_t * len) {
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &b->sheet->alloc;
  const GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(v, n, &so, out, len);
  if (st != GTEXT_CSS_OK) {
    return bfail(b, st, NULL, NULL, NULL);
  }
  return GTEXT_CSS_OK;
}

/// The arrays that go with `imports` (the node of each, the scope of each) are as long as it is.
static bool grow_import_aux(GTEXT_CSS_Sheet * s) {
  if (s->capimport_aux >= s->capimports) {
    return true;
  }
  uint32_t * ic = gtext_allocator_realloc(&s->alloc, s->import_cond, s->capimports * sizeof *ic);
  if (!ic) {
    return false;
  }
  s->import_cond = ic;
  const sheet_scope ** is = gtext_allocator_realloc(&s->alloc, (void *)s->import_scope, s->capimports * sizeof *is);
  if (!is) {
    return false;
  }
  s->import_scope = is;
  s->capimport_aux = s->capimports;
  return true;
}

static GTEXT_CSS_Status import_rule(build * b, const GTEXT_CSS_Rule * rule, bool open, bool * valid) {
  *valid = false;
  GTEXT_CSS_Sheet * s = b->sheet;
  const GTEXT_CSS_Component_Value * p = rule->prelude;
  const size_t n = rule->prelude_count;
  size_t i = 0;
  while (i < n && is_ws(&p[i])) {
    i++;
  }
  GTEXT_CSS_Import_Request rq;
  memset(&rq, 0, sizeof rq);
  rq.rule = rule;
  bool has_url = false;
  if (i < n && p[i].kind == GTEXT_CSS_VALUE_TOKEN && (p[i].type == GTEXT_CSS_TOKEN_URL || p[i].type == GTEXT_CSS_TOKEN_STRING)) {
    has_url = true;
    rq.url = p[i].value;
  }
  else if (i < n && p[i].kind == GTEXT_CSS_VALUE_FUNCTION && css_ascii_ieq(&p[i].value, "url") && p[i].child_count) {
    for (size_t k = 0; k < p[i].child_count; k++) {
      if (!is_ws(&p[i].children[k])) {
        has_url = true;
        rq.url = p[i].children[k].value;
        break;
      }
    }
  }
  if (!has_url) {
    return diag(b, GTEXT_CSS_SHEET_DIAG_INVALID_RULE, rule->span, rule->id, "CSS Cascading 5, 2.1: an @import needs a <url> or a <string>") ? GTEXT_CSS_OK : b->status;
  }
  i++;
  /* Cascade 5 and 6: `[ layer | layer(<name>) ] || [ scope | scope(...) ] || supports(...)` in any order, then the media query list. */
  const GTEXT_CSS_Component_Value *layer_v = NULL, *scope_v = NULL, *supports_v = NULL;
  for (; i < n; i++) {
    if (is_ws(&p[i])) {
      continue;
    }
    const bool kw = p[i].kind == GTEXT_CSS_VALUE_TOKEN && p[i].type == GTEXT_CSS_TOKEN_IDENT;
    const bool fn = p[i].kind == GTEXT_CSS_VALUE_FUNCTION;
    if (!layer_v && (kw || fn) && css_ascii_ieq(&p[i].value, "layer")) {
      layer_v = &p[i];
    }
    else if (!scope_v && (kw || fn) && css_ascii_ieq(&p[i].value, "scope")) {
      scope_v = &p[i];
    }
    else if (!supports_v && fn && css_ascii_ieq(&p[i].value, "supports")) {
      supports_v = &p[i];
    }
    else {
      break;
    }
  }
  size_t media_from = i;
  while (media_from < n && is_ws(&p[media_from])) {
    media_from++;
  }
  const bool has_media = media_from < n;
  char *layer_text = NULL, *sup_text = NULL, *media_text = NULL, *scope_text = NULL;
  size_t layer_len = 0, sup_len = 0, media_len = 0, scope_len = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (layer_v) {
    rq.layered = true;
    if (layer_v->kind == GTEXT_CSS_VALUE_FUNCTION) {
      st = text_of(b, layer_v->children, layer_v->child_count, &layer_text, &layer_len);
      rq.layer.data = layer_text;
      rq.layer.length = layer_len;
    }
    else {
      rq.layer_anonymous = true;
    }
  }
  if (st == GTEXT_CSS_OK && supports_v) {
    rq.has_supports = true;
    st = text_of(b, supports_v->children, supports_v->child_count, &sup_text, &sup_len);
    rq.supports.data = sup_text;
    rq.supports.length = sup_len;
  }
  if (st == GTEXT_CSS_OK && has_media) {
    rq.has_media = true;
    st = text_of(b, p + media_from, n - media_from, &media_text, &media_len);
    rq.media.data = media_text;
    rq.media.length = media_len;
  }
  if (st == GTEXT_CSS_OK && scope_v) {
    rq.has_scope = true;
    if (scope_v->kind == GTEXT_CSS_VALUE_FUNCTION) {
      st = text_of(b, scope_v->children, scope_v->child_count, &scope_text, &scope_len);
    }
    rq.scope.data = scope_text ? scope_text : "";
    rq.scope.length = scope_len;
  }
  GTEXT_Allocator * a = &s->alloc;
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = a;
  GTEXT_CSS_Status ret = st;
  GTEXT_CSS_Media_List *media_list = NULL, *sup_list = NULL;
  if (st == GTEXT_CSS_OK) {
    *valid = true;
    if (!open) {
      *valid = false;
      ret = diag(b, GTEXT_CSS_SHEET_DIAG_INVALID_RULE, rule->span, rule->id, "CSS Cascading 5, 2.1: an @import after any other rule is ignored") ? GTEXT_CSS_OK : b->status;
    }
    else {
      /* the conditions: parsed once, owned by the node, and handed to the callback to decide whether to fetch */
      GTEXT_CSS_Media_Options mo = condition_options(b);
      GTEXT_CSS_Media_Result mr;
      memset(&mr, 0, sizeof mr);
      if (has_media) {
        ret = gtext_css_media_list_parse_values(p + media_from, n - media_from, &mo, &media_list, &mr);
        gtext_css_media_result_free(&mr);
      }
      if (ret == GTEXT_CSS_OK && supports_v) {
        memset(&mr, 0, sizeof mr);
        ret = gtext_css_supports_parse_values(supports_v->children, supports_v->child_count, true, &mo, &sup_list, &mr);
        gtext_css_media_result_free(&mr);
      }
      uint32_t node = 0;
      if (ret == GTEXT_CSS_OK && (media_list || sup_list)) {
        ret = new_cond(b, SCK_IMPORT, 0, 0, rule->span, &node);
        if (ret == GTEXT_CSS_OK) {
          s->conds[node - 1].list = media_list;
          s->conds[node - 1].list2 = sup_list;
          media_list = sup_list = NULL; /* the node owns them */
        }
      }
      sheet_scope * isc = NULL;
      if (ret == GTEXT_CSS_OK && scope_v) {
        GTEXT_CSS_Selector_List *ss = NULL, *se = NULL;
        bool bad = false;
        if (scope_v->kind == GTEXT_CSS_VALUE_FUNCTION && scope_v->child_count) {
          const GTEXT_CSS_Component_Value * c = scope_v->children;
          size_t cn = scope_v->child_count, f = 0;
          while (f < cn && is_ws(&c[f])) {
            f++;
          }
          if (f < cn && ((c[f].kind == GTEXT_CSS_VALUE_BLOCK && c[f].type == GTEXT_CSS_TOKEN_OPEN_PAREN) ||
                            (c[f].kind == GTEXT_CSS_VALUE_TOKEN && c[f].type == GTEXT_CSS_TOKEN_IDENT && css_ascii_ieq(&c[f].value, "to")))) {
            ret = read_scope_boundaries(b, c, cn, false, false, rule->span, &ss, &se, &bad);
          }
          else if (f < cn) {
            ret = parse_selector(b, c, cn, false, false, rule->span, &ss);
            bad = ret == GTEXT_CSS_OK && (ss == NULL || list_ends_in_pseudo_element(ss));
            if (bad) {
              gtext_css_selector_list_free(ss);
              ss = NULL;
            }
          }
        }
        if (ret == GTEXT_CSS_OK && bad) {
          /* a scope() that is not valid makes the import invalid */
          ret = diag(b, GTEXT_CSS_SHEET_DIAG_INVALID_RULE, rule->span, rule->id, "CSS Cascading 6, 2.2: the argument of scope() is not valid: the import is invalid") ? GTEXT_CSS_OK : b->status;
          *valid = false;
          scope_v = NULL;
          goto finish;
        }
        if (ret == GTEXT_CSS_OK) {
          ret = make_scope(b, ss, se, NULL, NULL, rule->span, &isc);
        }
      }
      if (ret == GTEXT_CSS_OK && layer_v) {
        ret = layers_of(b, rule);
        if (ret == GTEXT_CSS_OK) {
          ret = emit_declarations(b, rule, node);
        }
      }
      if (ret == GTEXT_CSS_OK && b->opt->import) {
        rq.media_list = node ? s->conds[node - 1].list : NULL;
        rq.supports_condition = node ? s->conds[node - 1].list2 : NULL;
        GTEXT_CSS_Sheet * got = b->opt->import(b->opt->import_user, &rq);
        const uint32_t lnode = rq.layered ? sheet_layer_no(s, b->layer_of[rule->id]) : 0;
        if (!got) {
          if (!diag(b, GTEXT_CSS_SHEET_DIAG_IMPORT_FAILED, rule->span, rule->id, "CSS Cascading 5, 2.1: an import that cannot be fetched contributes nothing")) {
            ret = b->status;
          }
        }
        else if (got->registry != s->registry) {
          gtext_css_sheet_free(got);
          ret = bfail(b, GTEXT_CSS_E_INVALID, &rule->span, "import", "the imported sheet was compiled with another registry");
        }
        else {
          const size_t maxd = b->opt->max_import_depth ? (b->opt->max_import_depth > MAX_IMPORT_DEPTH ? MAX_IMPORT_DEPTH : b->opt->max_import_depth) : DEF_IMPORT_DEPTH;
          if (got->import_depth + 1 > maxd) {
            gtext_css_sheet_free(got);
            ret = bfail(b, GTEXT_CSS_E_LIMIT_RULE_NESTING, &rule->span, "max_import_depth", "imports are nested deeper than max_import_depth");
          }
          else if (!grow(a, (void **)&s->imports, &s->capimports, s->nimports + 1, sizeof *s->imports) || !grow_import_aux(s)) {
            gtext_css_sheet_free(got);
            ret = bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
          }
          else {
            const uint32_t idx = (uint32_t)s->nimports;
            s->imports[s->nimports] = got;
            s->import_cond[s->nimports] = node;
            s->import_scope[s->nimports] = isc;
            s->nimports++;
            if (got->import_depth + 1 > s->import_depth) {
              s->import_depth = got->import_depth + 1;
            }
            ret = add_event(b, 1, lnode, idx, node);
          }
        }
      }
    }
  }
finish:
  gtext_css_media_list_free(media_list);
  gtext_css_media_list_free(sup_list);
  gtext_css_serialize_buffer_free(&so, layer_text);
  gtext_css_serialize_buffer_free(&so, sup_text);
  gtext_css_serialize_buffer_free(&so, media_text);
  gtext_css_serialize_buffer_free(&so, scope_text);
  return ret;
}

static GTEXT_CSS_Status namespace_rule(build * b, const GTEXT_CSS_Rule * rule, bool open) {
  GTEXT_CSS_Sheet * s = b->sheet;
  const GTEXT_CSS_Component_Value * p = rule->prelude;
  const size_t n = rule->prelude_count;
  size_t i = 0;
  while (i < n && is_ws(&p[i])) {
    i++;
  }
  const GTEXT_CSS_Component_Value * prefix = NULL;
  if (i < n && p[i].kind == GTEXT_CSS_VALUE_TOKEN && p[i].type == GTEXT_CSS_TOKEN_IDENT) {
    prefix = &p[i++];
    while (i < n && is_ws(&p[i])) {
      i++;
    }
  }
  const GTEXT_CSS_String * uri = NULL;
  if (i < n && p[i].kind == GTEXT_CSS_VALUE_TOKEN && (p[i].type == GTEXT_CSS_TOKEN_STRING || p[i].type == GTEXT_CSS_TOKEN_URL)) {
    uri = &p[i].value;
    i++;
  }
  else if (i < n && p[i].kind == GTEXT_CSS_VALUE_FUNCTION && css_ascii_ieq(&p[i].value, "url") && p[i].child_count) {
    for (size_t k = 0; k < p[i].child_count && !uri; k++) {
      if (!is_ws(&p[i].children[k])) {
        uri = &p[i].children[k].value;
      }
    }
    i++;
  }
  while (i < n && is_ws(&p[i])) {
    i++;
  }
  if (!uri || i < n || !open) {
    return diag(b, GTEXT_CSS_SHEET_DIAG_INVALID_RULE, rule->span, rule->id, "CSS Namespaces 3, 2.1: an @namespace rule follows @charset, @import and @layer statements only, and has a string or url") ?
        GTEXT_CSS_OK :
        b->status;
  }
  char * u = css_arena_strdup(&s->arena, uri->data ? uri->data : "", uri->length);
  char * pf = prefix ? css_arena_strdup(&s->arena, prefix->value.data, prefix->value.length) : NULL;
  if (!u || (prefix && !pf)) {
    return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  if (!pf) {
    s->default_ns = u;
    return GTEXT_CSS_OK;
  }
  for (size_t k = 0; k < s->nns; k++) {
    if (strcmp(s->ns[k].prefix, pf) == 0) {
      s->ns[k].uri = u;
      return GTEXT_CSS_OK;
    }
  }
  GTEXT_CSS_Namespace * nn = gtext_allocator_realloc(&s->alloc, s->ns, (s->nns + 1) * sizeof *nn);
  if (!nn) {
    return bfail(b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  s->ns = nn;
  s->ns[s->nns].prefix = pf;
  s->ns[s->nns].uri = u;
  s->nns++;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Span zero_span(void) {
  GTEXT_CSS_Span s;
  memset(&s, 0, sizeof s);
  return s;
}

/// Run the stack until the frame it started with is done.
static GTEXT_CSS_Status drain(build * b, size_t base) {
  while (b->sp > base && b->status == GTEXT_CSS_OK) {
    frame * top = &b->stack[b->sp - 1];
    const GTEXT_CSS_Rule * next = NULL;
    if (top->items) {
      if (top->pos >= top->nitems) {
        b->sp--;
        continue;
      }
      const GTEXT_CSS_Block_Item * it = &top->items[top->pos++];
      if (it->kind == GTEXT_CSS_ITEM_DECLARATIONS) {
        const frame cur = *top;
        top->chain = 0; /* anything but a rule between two conditional group rules ends a chain */
        const GTEXT_CSS_Status st = declarations_of_parent(b, &cur, it->declarations, it->declaration_count, 0, zero_span());
        if (st != GTEXT_CSS_OK) {
          return st;
        }
        continue;
      }
      next = it->rule;
    }
    else {
      if (top->pos >= top->nrules) {
        b->sp--;
        continue;
      }
      next = &top->rules[top->pos++];
    }
    const frame cur = *top; // the stack may not move while the rule is read, but it is pushed to
    const GTEXT_CSS_Status st = visit(b, next, &cur);
    top->chain = b->chain_out; /* the frame's slot stays where it is: push() copies into the next slot */
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  return b->status;
}

GTEXT_CSS_Status gtext_css_sheet_compile(const GTEXT_CSS_Tree * tree, const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Sheet_Options * options,
    GTEXT_CSS_Sheet ** out, GTEXT_CSS_Sheet_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!tree || !registry || !out || (tree->kind != GTEXT_CSS_TREE_STYLESHEET && tree->kind != GTEXT_CSS_TREE_STYLESHEET_CONTENTS)) {
    if (result) {
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
      result->detail = "a NULL argument, or a tree that is not a style sheet";
    }
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Sheet_Options def;
  if (!options) {
    def = gtext_css_sheet_options_default();
    options = &def;
  }
  GTEXT_Allocator alloc = options->allocator ? *options->allocator : *css_registry_allocator(registry);
  build b;
  memset(&b, 0, sizeof b);
  b.opt = options;
  b.res = result;
  b.tree = tree;
  GTEXT_CSS_Sheet * s = gtext_allocator_calloc(&alloc, 1, sizeof *s);
  if (!s) {
    return bfail(&b, GTEXT_CSS_E_OOM, NULL, NULL, NULL);
  }
  gcu_atomic_int_init(&s->refs, 1);
  s->alloc = alloc;
  css_arena_init(&s->arena, &s->alloc);
  s->registry = registry;
  s->quirks = options->quirks;
  b.sheet = s;
  b.max_diag = options->max_diagnostics ? options->max_diagnostics : DEF_DIAGS;
  b.max_depth = options->max_rule_depth ? options->max_rule_depth : DEF_RULE_DEPTH;
  if (b.max_depth > MAX_RULE_DEPTH) {
    b.max_depth = MAX_RULE_DEPTH;
  }
  GTEXT_CSS_Layer_Options lopt = gtext_css_layer_options_default();
  lopt.allocator = &s->alloc;
  GTEXT_CSS_Status st = gtext_css_layer_registry_new(&lopt, &s->layers);
  b.stack = gtext_allocator_calloc(&s->alloc, b.max_depth + 1, sizeof *b.stack);
  b.layer_of = gtext_allocator_calloc(&s->alloc, tree->node_count + 1, sizeof *b.layer_of);
  b.decl_first = gtext_allocator_calloc(&s->alloc, tree->node_count + 1, sizeof *b.decl_first);
  b.decl_count = gtext_allocator_calloc(&s->alloc, tree->node_count + 1, sizeof *b.decl_count);
  if (st != GTEXT_CSS_OK || !b.stack || !b.layer_of || !b.decl_first || !b.decl_count) {
    bfail(&b, st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_OOM, NULL, NULL, NULL);
    goto done;
  }
  {
    frame top;
    memset(&top, 0, sizeof top);
    top.nspec = gtext_css_specificity_make(0, 1, 0);
    bool imports_open = true, ns_open = true;
    uint32_t top_chain = 0;
    for (size_t i = 0; i < tree->rule_count && b.status == GTEXT_CSS_OK; i++) {
      const GTEXT_CSS_Rule * r = &tree->rules[i];
      if (r->kind == GTEXT_CSS_RULE_AT && css_ascii_ieq(&r->name, "charset")) {
        continue;
      }
      if (r->kind == GTEXT_CSS_RULE_AT && css_ascii_ieq(&r->name, "import")) {
        top_chain = 0;
        bool valid = false;
        st = import_rule(&b, r, imports_open, &valid);
        if (st != GTEXT_CSS_OK) {
          break;
        }
        continue;
      }
      if (r->kind == GTEXT_CSS_RULE_AT && css_ascii_ieq(&r->name, "namespace")) {
        top_chain = 0;
        imports_open = false;
        st = namespace_rule(&b, r, ns_open);
        continue;
      }
      const bool statement = r->kind == GTEXT_CSS_RULE_AT && css_ascii_ieq(&r->name, "layer") && !r->has_block;
      if (!statement) {
        imports_open = false;
        ns_open = false;
      }
      if (r->kind == GTEXT_CSS_RULE_AT || r->rule_count) {
        st = layers_of(&b, r);
        if (st != GTEXT_CSS_OK) {
          break;
        }
      }
      frame one = top;
      one.rules = r;
      one.nrules = 1;
      one.chain = top_chain;
      if (b.sp != 0) {
        bfail(&b, GTEXT_CSS_E_INTERNAL, NULL, NULL, "the rule stack is not empty");
        break;
      }
      st = push(&b, &one, &r->span);
      if (st == GTEXT_CSS_OK) {
        st = drain(&b, 0);
        top_chain = b.stack[0].chain;
      }
    }
  }
done:
  gtext_allocator_free(&s->alloc, b.stack);
  gtext_allocator_free(&s->alloc, b.layer_of);
  gtext_allocator_free(&s->alloc, b.decl_first);
  gtext_allocator_free(&s->alloc, b.decl_count);
  gtext_allocator_free(&s->alloc, b.decl_layers);
  if (b.status != GTEXT_CSS_OK) {
    sheet_destroy(s);
    return b.status;
  }
  *out = s;
  return GTEXT_CSS_OK;
}

/* ---- reading a sheet ---------------------------------------------------------------------------------------------- */

size_t gtext_css_sheet_record_count(const GTEXT_CSS_Sheet * sheet) {
  return sheet ? sheet->nrecs : 0;
}

bool gtext_css_sheet_record(const GTEXT_CSS_Sheet * sheet, size_t index, GTEXT_CSS_Sheet_Record * out) {
  if (!sheet || !out || index >= sheet->nrecs) {
    return false;
  }
  const sheet_record * r = &sheet->recs[index];
  memset(out, 0, sizeof *out);
  out->selector = r->list;
  out->complex_index = r->cx;
  out->specificity = r->spec;
  out->rule = r->run;
  out->declaration_count = sheet->runs[r->run].ndecl;
  out->layer = r->layer;
  out->nested = (r->flags & REC_NESTED) != 0;
  out->scope_depth = r->scope ? r->scope->depth : 0;
  out->dependencies = r->list->complex[r->cx].dependencies;
  out->condition = r->cond;
  out->span = sheet->runs[r->run].span;
  return true;
}

size_t gtext_css_sheet_record_declarations(const GTEXT_CSS_Sheet * sheet, size_t index, GTEXT_CSS_Property_Id * properties, bool * important, size_t capacity) {
  if (!sheet || index >= sheet->nrecs) {
    return 0;
  }
  const sheet_run * run = &sheet->runs[sheet->recs[index].run];
  for (size_t i = 0; i < run->ndecl && i < capacity; i++) {
    if (properties) {
      properties[i] = run->decls[i].custom ? 0 : run->decls[i].pv.property;
    }
    if (important) {
      important[i] = run->decls[i].important;
    }
  }
  return run->ndecl;
}

size_t gtext_css_sheet_layer_count(const GTEXT_CSS_Sheet * sheet) {
  return sheet ? gtext_css_layer_count(sheet->layers, GTEXT_CSS_ORIGIN_AUTHOR) : 0;
}

size_t gtext_css_sheet_layer_path(const GTEXT_CSS_Sheet * sheet, uint32_t layer, char * buffer, size_t capacity) {
  return sheet ? gtext_css_layer_path(sheet->layers, sheet_layer_id(sheet, layer), buffer, capacity) : 0;
}

size_t gtext_css_sheet_import_count(const GTEXT_CSS_Sheet * sheet) {
  return sheet ? sheet->nimports : 0;
}

const GTEXT_CSS_Sheet * gtext_css_sheet_import(const GTEXT_CSS_Sheet * sheet, size_t index) {
  return sheet && index < sheet->nimports ? sheet->imports[index] : NULL;
}

size_t gtext_css_sheet_diagnostic_total(const GTEXT_CSS_Sheet * sheet) {
  return sheet ? sheet->diag_total : 0;
}

bool gtext_css_sheet_diagnostic(const GTEXT_CSS_Sheet * sheet, size_t index, GTEXT_CSS_Sheet_Diagnostic * out) {
  if (!sheet || !out || index >= sheet->ndiags) {
    return false;
  }
  *out = sheet->diags[index];
  return true;
}

size_t gtext_css_sheet_condition_count(const GTEXT_CSS_Sheet * sheet) {
  return sheet ? sheet->nconds : 0;
}

bool gtext_css_sheet_condition(const GTEXT_CSS_Sheet * sheet, uint32_t id, GTEXT_CSS_Sheet_Condition * out) {
  if (!sheet || !out || id == 0 || id > sheet->nconds) {
    return false;
  }
  const sheet_cond * c = &sheet->conds[id - 1];
  memset(out, 0, sizeof *out);
  out->kind = (GTEXT_CSS_Sheet_Condition_Kind)c->kind;
  out->parent = c->parent;
  out->previous = c->prev;
  out->else_rule = (c->flags & SCF_ELSE) != 0;
  out->always = c->kind == SCK_WHEN && c->list == NULL;
  out->element_dependent = (c->flags & SCF_ELEMENT) != 0;
  out->starting_style = (c->flags & SCF_STARTING) != 0;
  out->list = c->list;
  out->supports = c->list2;
  out->span = c->span;
  return true;
}

uint32_t gtext_css_sheet_import_condition(const GTEXT_CSS_Sheet * sheet, size_t index) {
  return sheet && index < sheet->nimports ? sheet->import_cond[index] : 0;
}

/* ---- the dump ------------------------------------------------------------------------------------------------------ */

static GTEXT_CSS_Status put(const GTEXT_Markup_Sink * sink, const char * s, size_t n) {
  return n && sink->write(sink->user, s, n) != GTEXT_MARKUP_OK ? GTEXT_CSS_E_WRITE : GTEXT_CSS_OK;
}

static GTEXT_CSS_Status putz(const GTEXT_Markup_Sink * sink, const char * s) {
  return put(sink, s, strlen(s));
}

static GTEXT_CSS_Status putu(const GTEXT_Markup_Sink * sink, uint64_t v) {
  char buf[24];
  size_t i = sizeof buf;
  do {
    buf[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  return put(sink, buf + i, sizeof buf - i);
}

GTEXT_CSS_Status gtext_css_sheet_dump(const GTEXT_CSS_Sheet * sheet, const GTEXT_Markup_Sink * sink) {
  if (!sheet || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = putz(sink, "sheet 1\n");
  for (size_t i = 0; st == GTEXT_CSS_OK && i < sheet->nns; i++) {
    st = putz(sink, "namespace ");
    st = st ? st : putz(sink, sheet->ns[i].prefix);
    st = st ? st : putz(sink, " ");
    st = st ? st : putz(sink, sheet->ns[i].uri);
    st = st ? st : putz(sink, "\n");
  }
  if (st == GTEXT_CSS_OK && sheet->default_ns) {
    st = putz(sink, "namespace * ");
    st = st ? st : putz(sink, sheet->default_ns);
    st = st ? st : putz(sink, "\n");
  }
  const size_t nl = gtext_css_sheet_layer_count(sheet);
  for (size_t i = 1; st == GTEXT_CSS_OK && i <= nl; i++) {
    char path[512];
    gtext_css_sheet_layer_path(sheet, (uint32_t)i, path, sizeof path);
    st = putz(sink, "layer ");
    st = st ? st : putu(sink, i);
    st = st ? st : putz(sink, " ");
    st = st ? st : putz(sink, path);
    st = st ? st : putz(sink, "\n");
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < sheet->nimports; i++) {
    st = putz(sink, "import ");
    st = st ? st : putu(sink, i);
    st = st ? st : putz(sink, " records=");
    st = st ? st : putu(sink, sheet->imports[i]->nrecs);
    if (st == GTEXT_CSS_OK && sheet->import_cond[i]) {
      st = putz(sink, " cond=");
      st = st ? st : putu(sink, sheet->import_cond[i]);
    }
    if (st == GTEXT_CSS_OK && sheet->import_scope[i]) {
      st = putz(sink, " scope");
    }
    st = st ? st : putz(sink, "\n");
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < sheet->nconds; i++) {
    static const char * const names[] = {"", "media", "supports", "container", "starting-style", "when", "never", "import"};
    const sheet_cond * c = &sheet->conds[i];
    st = putz(sink, "cond ");
    st = st ? st : putu(sink, i + 1);
    st = st ? st : putz(sink, " ");
    st = st ? st : putz(sink, (c->flags & SCF_ELSE) ? "else" : names[c->kind]);
    if (st == GTEXT_CSS_OK && c->parent) {
      st = putz(sink, " parent=");
      st = st ? st : putu(sink, c->parent);
    }
    if (st == GTEXT_CSS_OK && c->prev) {
      st = putz(sink, " prev=");
      st = st ? st : putu(sink, c->prev);
    }
    if (st == GTEXT_CSS_OK && c->list2) {
      st = putz(sink, " supports(");
      const GTEXT_CSS_Status w = gtext_css_media_list_serialize(c->list2, sink);
      st = w != GTEXT_CSS_OK ? w : putz(sink, ")");
    }
    if (st == GTEXT_CSS_OK && c->list) {
      st = putz(sink, " ");
      const GTEXT_CSS_Status w = gtext_css_media_list_serialize(c->list, sink);
      st = w != GTEXT_CSS_OK ? w : st;
    }
    st = st ? st : putz(sink, "\n");
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < sheet->nevents; i++) {
    const sheet_event * ev = &sheet->events[i];
    if (!ev->cond) {
      continue;
    }
    st = putz(sink, ev->kind == 0 ? "event layer " : "event import ");
    if (ev->kind == 0) {
      char path[512];
      gtext_css_sheet_layer_path(sheet, ev->node, path, sizeof path);
      st = st ? st : putz(sink, path);
    }
    else {
      st = st ? st : putu(sink, ev->import);
    }
    st = st ? st : putz(sink, " cond=");
    st = st ? st : putu(sink, ev->cond);
    st = st ? st : putz(sink, "\n");
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < sheet->nrecs; i++) {
    const sheet_record * r = &sheet->recs[i];
    st = putz(sink, "record ");
    st = st ? st : putu(sink, i);
    st = st ? st : putz(sink, " ");
    st = st ? st : gtext_css_selector_serialize_complex(&r->list->complex[r->cx], sink);
    char path[512];
    gtext_css_sheet_layer_path(sheet, r->layer, path, sizeof path);
    st = st ? st : putz(sink, " spec=");
    st = st ? st : putu(sink, r->spec.a);
    st = st ? st : putz(sink, ",");
    st = st ? st : putu(sink, r->spec.b);
    st = st ? st : putz(sink, ",");
    st = st ? st : putu(sink, r->spec.c);
    st = st ? st : putz(sink, " rule=");
    st = st ? st : putu(sink, r->run);
    st = st ? st : putz(sink, " layer=");
    st = st ? st : putz(sink, path);
    st = st ? st : putz(sink, " scope=");
    st = st ? st : putu(sink, r->scope ? r->scope->depth : 0);
    st = st ? st : putz(sink, r->flags & REC_NESTED ? " nested" : "");
    if (st == GTEXT_CSS_OK && r->cond) {
      st = putz(sink, " cond=");
      st = st ? st : putu(sink, r->cond);
    }
    st = st ? st : putz(sink, "\n");
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < sheet->nruns; i++) {
    const sheet_run * run = &sheet->runs[i];
    for (size_t j = 0; st == GTEXT_CSS_OK && j < run->ndecl; j++) {
      const sheet_decl * d = &run->decls[j];
      st = putz(sink, "rule ");
      st = st ? st : putu(sink, i);
      st = st ? st : putz(sink, " ");
      st = st ? st : put(sink, d->name, d->name_len);
      st = st ? st : putz(sink, ": ");
      st = st ? st : gtext_css_property_value_serialize(&d->pv, NULL, sink);
      st = st ? st : putz(sink, d->important ? " !important\n" : "\n");
    }
  }
  return st;
}
