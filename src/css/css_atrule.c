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
 * The typed model of an at-rule (css_atrule.h): the prelude of each kind, the descriptors and declarations of its block validated through the registry, the required
 * descriptors and the allowed places from the generated tables, and CSSOM's serialization. One iterative pass over the rule's block; the nested rules (keyframe blocks, margin
 * rules, feature blocks) are rules of their own, parsed by the same function one level down (their depth is the depth of the grammar, at most two).
 */

#include <string.h>

#include "css_atrule_internal.h"
#include "css_atrule_tables.h"
#include "css_registry_internal.h"

#define DEF_DESCRIPTORS ((size_t)4096)
#define DEF_CHILDREN ((size_t)65536)
#define DEF_DIAGS ((size_t)1024)
#define DEF_LIST ((size_t)65536)

GTEXT_CSS_Atrule_Options gtext_css_atrule_options_default(void) {
  GTEXT_CSS_Atrule_Options o;
  memset(&o, 0, sizeof o);
  o.max_descriptors = DEF_DESCRIPTORS;
  o.max_children = DEF_CHILDREN;
  o.max_diagnostics = DEF_DIAGS;
  o.max_list = DEF_LIST;
  return o;
}

static const char * const kind_names[GTEXT_CSS_ATRULE_KIND_COUNT] = {
  [GTEXT_CSS_ATRULE_UNKNOWN] = "unknown",
  [GTEXT_CSS_ATRULE_CHARSET] = "charset",
  [GTEXT_CSS_ATRULE_IMPORT] = "import",
  [GTEXT_CSS_ATRULE_NAMESPACE] = "namespace",
  [GTEXT_CSS_ATRULE_LAYER] = "layer",
  [GTEXT_CSS_ATRULE_SCOPE] = "scope",
  [GTEXT_CSS_ATRULE_PROPERTY] = "property",
  [GTEXT_CSS_ATRULE_MEDIA] = "media",
  [GTEXT_CSS_ATRULE_SUPPORTS] = "supports",
  [GTEXT_CSS_ATRULE_CONTAINER] = "container",
  [GTEXT_CSS_ATRULE_STARTING_STYLE] = "starting-style",
  [GTEXT_CSS_ATRULE_WHEN] = "when",
  [GTEXT_CSS_ATRULE_ELSE] = "else",
  [GTEXT_CSS_ATRULE_CUSTOM_MEDIA] = "custom-media",
  [GTEXT_CSS_ATRULE_FONT_FACE] = "font-face",
  [GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES] = "font-feature-values",
  [GTEXT_CSS_ATRULE_FEATURE_BLOCK] = "feature-block",
  [GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES] = "font-palette-values",
  [GTEXT_CSS_ATRULE_COLOR_PROFILE] = "color-profile",
  [GTEXT_CSS_ATRULE_COUNTER_STYLE] = "counter-style",
  [GTEXT_CSS_ATRULE_PAGE] = "page",
  [GTEXT_CSS_ATRULE_PAGE_MARGIN] = "page-margin",
  [GTEXT_CSS_ATRULE_KEYFRAMES] = "keyframes",
  [GTEXT_CSS_ATRULE_KEYFRAME] = "keyframe",
  [GTEXT_CSS_ATRULE_VIEW_TRANSITION] = "view-transition",
  [GTEXT_CSS_ATRULE_POSITION_TRY] = "position-try",
  [GTEXT_CSS_ATRULE_PRESERVED] = "preserved",
};

const char * gtext_css_atrule_kind_name(GTEXT_CSS_Atrule_Kind kind) {
  return (unsigned)kind < (unsigned)GTEXT_CSS_ATRULE_KIND_COUNT ? kind_names[kind] : "unknown";
}

/* ---- the ledger and the rows ------------------------------------------------------------------------------------- */

static int ci_cmp(const char * a, size_t an, const char * b) {
  const size_t bn = strlen(b);
  const size_t n = an < bn ? an : bn;
  for (size_t i = 0; i < n; i++) {
    unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
    if (x >= 'A' && x <= 'Z') {
      x = (unsigned char)(x + 32);
    }
    if (x != y) {
      return x < y ? -1 : 1;
    }
  }
  return an == bn ? 0 : (an < bn ? -1 : 1);
}

const void * css_atrule_row_find(const char * name, size_t length) {
  size_t lo = 0, hi = CSS_ATRULE_ROW_COUNT;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = ci_cmp(name, length, css_atrule_rows[mid].name);
    if (c == 0) {
      return &css_atrule_rows[mid];
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return NULL;
}

GTEXT_CSS_Atrule_Kind css_atrule_row_kind(const void * row) {
  return (GTEXT_CSS_Atrule_Kind)((const css_atrule_row *)row)->kind;
}

size_t gtext_css_atrule_ledger_count(void) {
  return CSS_ATRULE_ROW_COUNT;
}

bool gtext_css_atrule_ledger_row(size_t index, const char ** name, bool * parses, bool * validates, bool * serializes, bool * applies, bool * typed, bool * preserved) {
  if (index >= CSS_ATRULE_ROW_COUNT) {
    return false;
  }
  const css_atrule_row * r = &css_atrule_rows[index];
  if (name) {
    *name = r->name;
  }
  if (parses) {
    *parses = (r->flags & CSS_AR_F_PARSES) != 0;
  }
  if (validates) {
    *validates = (r->flags & CSS_AR_F_VALIDATES) != 0;
  }
  if (serializes) {
    *serializes = (r->flags & CSS_AR_F_SERIALIZES) != 0;
  }
  if (applies) {
    *applies = (r->flags & CSS_AR_F_APPLIES) != 0;
  }
  if (typed) {
    *typed = (r->flags & CSS_AR_F_TYPED) != 0;
  }
  if (preserved) {
    *preserved = (r->flags & CSS_AR_F_PRESERVED) != 0;
  }
  return true;
}

/* ---- the object ---------------------------------------------------------------------------------------------------- */

static void atrule_destroy(GTEXT_CSS_Atrule * a) {
  for (size_t i = 0; i < a->nds; i++) {
    gtext_css_property_value_free(&a->ds[i].pv);
  }
  for (size_t i = 0; i < a->nkids; i++) {
    gtext_css_atrule_free(a->kids[i]);
  }
  const GTEXT_Allocator al = a->alloc;
  gtext_allocator_free(&al, a->ds);
  gtext_allocator_free(&al, a->kids);
  gtext_allocator_free(&al, a->diags);
  gtext_allocator_free(&al, a->names);
  gtext_allocator_free(&al, a->quoted);
  gtext_allocator_free(&al, a->offsets);
  gtext_allocator_free(&al, a->sels);
  css_arena_free(&a->arena);
  gtext_allocator_free(&al, a);
}

GTEXT_CSS_Atrule * gtext_css_atrule_retain(const GTEXT_CSS_Atrule * rule) {
  if (rule) {
    (void)gcu_atomic_int_fetch_add((GCU_Atomic_Int *)&rule->refs, 1);
  }
  return (GTEXT_CSS_Atrule *)rule;
}

void gtext_css_atrule_free(const GTEXT_CSS_Atrule * rule) {
  if (!rule) {
    return;
  }
  GTEXT_CSS_Atrule * a = (GTEXT_CSS_Atrule *)rule;
  if (gcu_atomic_int_fetch_sub(&a->refs, 1) == 1) {
    atrule_destroy(a);
  }
}

GTEXT_CSS_Atrule_Kind gtext_css_atrule_kind(const GTEXT_CSS_Atrule * rule) {
  return rule ? rule->kind : GTEXT_CSS_ATRULE_UNKNOWN;
}

const char * gtext_css_atrule_name(const GTEXT_CSS_Atrule * rule, size_t * length) {
  if (length) {
    *length = rule ? rule->name_len : 0;
  }
  return rule ? rule->name : "";
}

GTEXT_CSS_Span gtext_css_atrule_span(const GTEXT_CSS_Atrule * rule) {
  GTEXT_CSS_Span s;
  memset(&s, 0, sizeof s);
  return rule ? rule->span : s;
}

const char * gtext_css_atrule_prelude_text(const GTEXT_CSS_Atrule * rule, size_t * length) {
  if (length) {
    *length = rule ? rule->prelude_len : 0;
  }
  return rule && rule->prelude ? rule->prelude : "";
}

size_t gtext_css_atrule_name_count(const GTEXT_CSS_Atrule * rule) {
  return rule ? rule->nnames : 0;
}

bool gtext_css_atrule_name_at(const GTEXT_CSS_Atrule * rule, size_t index, GTEXT_CSS_String * out, bool * quoted) {
  if (!rule || index >= rule->nnames) {
    return false;
  }
  if (out) {
    *out = rule->names[index];
  }
  if (quoted) {
    *quoted = rule->quoted[index];
  }
  return true;
}

size_t gtext_css_atrule_descriptor_count(const GTEXT_CSS_Atrule * rule) {
  return rule ? rule->nds : 0;
}

static void fill_desc(const atrule_desc * d, GTEXT_CSS_Atrule_Descriptor * out) {
  memset(out, 0, sizeof *out);
  out->name = d->name;
  out->name_length = d->name_len;
  out->descriptor = d->desc;
  out->property = d->property;
  out->property_id = d->prop;
  out->important = d->important;
  out->set = d->pv.set;
  out->text.data = d->text;
  out->text.length = d->text_len;
  out->declaration = d->decl;
  out->span = d->span;
}

bool gtext_css_atrule_descriptor_at(const GTEXT_CSS_Atrule * rule, size_t index, GTEXT_CSS_Atrule_Descriptor * out) {
  if (!rule || !out || index >= rule->nds) {
    return false;
  }
  fill_desc(&rule->ds[index], out);
  return true;
}

int css_atrule_find_index(const GTEXT_CSS_Atrule * a, const char * name, size_t length) {
  for (size_t i = 0; i < a->nds; i++) {
    const atrule_desc * d = &a->ds[i];
    if (d->name_len != length) {
      continue;
    }
    const bool custom = length > 2 && d->name[0] == '-' && d->name[1] == '-';
    if (custom ? memcmp(d->name, name, length) == 0 : ci_cmp(name, length, d->name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

GTEXT_CSS_Status css_atrule_tokens(const GTEXT_CSS_Atrule * a, const char * name, GTEXT_CSS_Tree ** tree) {
  *tree = NULL;
  const int i = css_atrule_find_index(a, name, strlen(name));
  if (i < 0) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Parse_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &a->alloc;
  GTEXT_CSS_Result pr;
  memset(&pr, 0, sizeof pr);
  const GTEXT_CSS_Status st = gtext_css_parse_component_values(a->ds[i].text, a->ds[i].text_len, &po, tree, &pr);
  gtext_css_result_free(&pr);
  return st;
}

const atrule_desc * css_atrule_desc_at(const GTEXT_CSS_Atrule * a, size_t i) {
  return i < a->nds ? &a->ds[i] : NULL;
}

bool gtext_css_atrule_descriptor_find(const GTEXT_CSS_Atrule * rule, const char * name, size_t length, GTEXT_CSS_Atrule_Descriptor * out) {
  if (!rule || !name || !out) {
    return false;
  }
  const int i = css_atrule_find_index(rule, name, length);
  if (i < 0) {
    return false;
  }
  fill_desc(&rule->ds[i], out);
  return true;
}

size_t gtext_css_atrule_child_count(const GTEXT_CSS_Atrule * rule) {
  return rule ? rule->nkids : 0;
}

const GTEXT_CSS_Atrule * gtext_css_atrule_child_at(const GTEXT_CSS_Atrule * rule, size_t index) {
  return rule && index < rule->nkids ? rule->kids[index] : NULL;
}

bool gtext_css_atrule_defines_nothing(const GTEXT_CSS_Atrule * rule) {
  return rule && rule->defines_nothing;
}

size_t gtext_css_atrule_diagnostic_total(const GTEXT_CSS_Atrule * rule) {
  return rule ? rule->diag_total : 0;
}

bool gtext_css_atrule_diagnostic(const GTEXT_CSS_Atrule * rule, size_t index, GTEXT_CSS_Atrule_Diagnostic * out) {
  if (!rule || !out || index >= rule->ndiags) {
    return false;
  }
  *out = rule->diags[index];
  return true;
}

/* ---- the parse context ------------------------------------------------------------------------------------------------ */

typedef struct {
  const GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Atrule_Options opt;
  GTEXT_CSS_Atrule * a;
  const GTEXT_CSS_Rule * rule;
  GTEXT_CSS_Atrule_Result * res;
  GTEXT_CSS_Status st;
  bool valid;
  size_t max_diag;
} ctx;

static bool grow(const GTEXT_Allocator * al, void ** p, size_t * cap, size_t need, size_t elem) {
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
  void * q = gtext_allocator_realloc(al, *p, n * elem);
  if (!q) {
    return false;
  }
  *p = q;
  *cap = n;
  return true;
}

static GTEXT_CSS_Status fail(ctx * c, GTEXT_CSS_Status st, const GTEXT_CSS_Span * span, const char * name) {
  if (c->st == GTEXT_CSS_OK) {
    c->st = st;
  }
  if (c->res) {
    c->res->status = st;
    c->res->message = gtext_css_status_string(st);
    if (span) {
      c->res->span = *span;
    }
    if (name) {
      css_copy_name(c->res->name, sizeof c->res->name, name);
    }
  }
  return st;
}

static bool diag(ctx * c, GTEXT_CSS_Atrule_Diag_Kind kind, GTEXT_CSS_Span span, const char * reference) {
  GTEXT_CSS_Atrule * a = c->a;
  a->diag_total++;
  if (a->ndiags >= c->max_diag) {
    return true;
  }
  if (!grow(&a->alloc, (void **)&a->diags, &a->capdiags, a->ndiags + 1, sizeof *a->diags)) {
    fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    return false;
  }
  a->diags[a->ndiags].kind = kind;
  a->diags[a->ndiags].span = span;
  a->diags[a->ndiags].reference = reference;
  a->ndiags++;
  return true;
}

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool only_ws(const GTEXT_CSS_Component_Value * v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!is_ws(&v[i])) {
      return false;
    }
  }
  return true;
}

static bool lower_eq(const GTEXT_CSS_String * s, const char * lit) {
  return css_ascii_ieq(s, lit);
}

static bool wide_keyword_name(const GTEXT_CSS_String * s) {
  return lower_eq(s, "initial") || lower_eq(s, "inherit") || lower_eq(s, "unset") || lower_eq(s, "revert") || lower_eq(s, "revert-layer") || lower_eq(s, "default");
}

/// The canonical text of component values, in the arena.
static char * text_in_arena(ctx * c, const GTEXT_CSS_Component_Value * v, size_t n, size_t * len) {
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &c->a->alloc;
  char * buf = NULL;
  size_t bl = 0;
  const GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(v, n, &so, &buf, &bl);
  if (st != GTEXT_CSS_OK) {
    fail(c, st, NULL, NULL);
    return NULL;
  }
  size_t s = 0;
  while (s < bl && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\n')) {
    s++;
  }
  size_t e = bl;
  while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t' || buf[e - 1] == '\n')) {
    e--;
  }
  char * out = css_arena_strdup(&c->a->arena, buf + s, e - s);
  gtext_css_serialize_buffer_free(&so, buf);
  if (!out) {
    fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    return NULL;
  }
  *len = e - s;
  return out;
}

/* ---- names -------------------------------------------------------------------------------------------------------- */

static bool add_name(ctx * c, const GTEXT_CSS_String * s, bool quoted) {
  GTEXT_CSS_Atrule * a = c->a;
  size_t cap = a->nnames;
  if (!grow(&a->alloc, (void **)&a->names, &cap, a->nnames + 1, sizeof *a->names)) {
    fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    return false;
  }
  size_t cap2 = a->nnames;
  if (!grow(&a->alloc, (void **)&a->quoted, &cap2, a->nnames + 1, sizeof *a->quoted)) {
    fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    return false;
  }
  char * copy = css_arena_strdup(&a->arena, s->data ? s->data : "", s->length);
  if (!copy) {
    fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    return false;
  }
  a->names[a->nnames].data = copy;
  a->names[a->nnames].length = s->length;
  a->quoted[a->nnames] = quoted;
  a->nnames++;
  return true;
}

/// One `<ident>` (or `<dashed-ident>`) prelude: the name. Returns false when the prelude is not that.
static bool single_ident(const GTEXT_CSS_Component_Value * p, size_t n, const GTEXT_CSS_Component_Value ** out) {
  const GTEXT_CSS_Component_Value * only = NULL;
  for (size_t i = 0; i < n; i++) {
    if (is_ws(&p[i])) {
      continue;
    }
    if (only || p[i].kind != GTEXT_CSS_VALUE_TOKEN || p[i].type != GTEXT_CSS_TOKEN_IDENT) {
      return false;
    }
    only = &p[i];
  }
  *out = only;
  return only != NULL;
}

/// `<font-family-name># = [ <string> | <custom-ident>+ ]#`: each entry's name (the idents joined by one space) in the names.
static bool family_names(ctx * c, const GTEXT_CSS_Component_Value * p, size_t n, bool *valid) {
  size_t start = 0;
  *valid = true;
  for (;;) {
    size_t end = start;
    while (end < n && !is_comma(&p[end])) {
      end++;
    }
    const GTEXT_CSS_Component_Value * toks[64];
    size_t nt = 0;
    for (size_t i = start; i < end; i++) {
      if (is_ws(&p[i])) {
        continue;
      }
      if (nt == 64) {
        *valid = false;
        return true;
      }
      toks[nt++] = &p[i];
    }
    if (nt == 0) {
      *valid = false;
      return true;
    }
    if (nt == 1 && toks[0]->kind == GTEXT_CSS_VALUE_TOKEN && toks[0]->type == GTEXT_CSS_TOKEN_STRING) {
      if (!add_name(c, &toks[0]->value, true)) {
        return false;
      }
    }
    else {
      size_t total = 0;
      for (size_t i = 0; i < nt; i++) {
        if (toks[i]->kind != GTEXT_CSS_VALUE_TOKEN || toks[i]->type != GTEXT_CSS_TOKEN_IDENT || wide_keyword_name(&toks[i]->value)) {
          *valid = false;
          return true;
        }
        total += toks[i]->value.length + 1;
      }
      char * joined = gtext_allocator_malloc(&c->a->alloc, total);
      if (!joined) {
        fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
        return false;
      }
      size_t k = 0;
      for (size_t i = 0; i < nt; i++) {
        if (i) {
          joined[k++] = ' ';
        }
        memcpy(joined + k, toks[i]->value.data, toks[i]->value.length);
        k += toks[i]->value.length;
      }
      GTEXT_CSS_String js = {joined, k};
      const bool ok = add_name(c, &js, false);
      gtext_allocator_free(&c->a->alloc, joined);
      if (!ok) {
        return false;
      }
    }
    if (end >= n) {
      break;
    }
    start = end + 1;
  }
  return true;
}

static bool counter_style_name_ok(const GTEXT_CSS_String * s) {
  return !lower_eq(s, "none") && !wide_keyword_name(s);
}

/// The predefined counter style names are lowercased on parse.
static bool predefined_counter_style(const GTEXT_CSS_String * s) {
  size_t lo = 0, hi = CSS_COUNTER_STYLE_NAME_COUNT;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int cmp = ci_cmp(s->data, s->length, css_counter_style_names[mid]);
    if (cmp == 0) {
      return true;
    }
    if (cmp < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return false;
}

/* ---- descriptors ------------------------------------------------------------------------------------------------------ */

static void free_desc(atrule_desc * d) {
  gtext_css_property_value_free(&d->pv);
  memset(d, 0, sizeof *d);
}

/// Keep a valid descriptor or declaration: a later one of a name replaces the earlier and takes its place at the end (CSSOM's declaration block).
static bool add_desc(ctx * c, atrule_desc * d) {
  GTEXT_CSS_Atrule * a = c->a;
  int old = css_atrule_find_index(a, d->name, d->name_len);
  if (old >= 0) {
    free_desc(&a->ds[old]);
    memmove(&a->ds[old], &a->ds[old + 1], (a->nds - (size_t)old - 1) * sizeof *a->ds);
    a->nds--;
  }
  if (a->nds >= c->opt.max_descriptors) {
    gtext_css_property_value_free(&d->pv);
    fail(c, GTEXT_CSS_E_LIMIT_DESCRIPTORS, &d->span, "max_descriptors");
    return false;
  }
  if (!grow(&a->alloc, (void **)&a->ds, &a->capds, a->nds + 1, sizeof *a->ds)) {
    gtext_css_property_value_free(&d->pv);
    fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    return false;
  }
  a->ds[a->nds++] = *d;
  return true;
}

static bool pattern_matches(const char * pat, const char * name, size_t len) {
  const size_t pn = strlen(pat);
  if (pn && pat[pn - 1] == '*') {
    if (len < pn - 1) {
      return false;
    }
    char prefix[64];
    if (pn - 1 >= sizeof prefix) {
      return false;
    }
    memcpy(prefix, pat, pn - 1);
    prefix[pn - 1] = 0;
    return ci_cmp(name, pn - 1, prefix) == 0;
  }
  return ci_cmp(name, len, pat) == 0;
}

/// Whether a block of the at-rule takes a declaration of the property (the generated allow / ignore / keep rows).
static bool property_taken(const char * atrule, const char * name, size_t len) {
  bool has_allow = false, allowed = false, ignored = false, kept = false;
  for (size_t i = 0; i < CSS_ATRULE_PROPERTY_RULE_COUNT; i++) {
    const css_atrule_property_rule * r = &css_atrule_property_rules[i];
    if (strcmp(r->atrule, atrule) != 0) {
      continue;
    }
    const bool m = pattern_matches(r->pattern, name, len);
    if (r->mode == 0) {
      has_allow = true;
      allowed = allowed || m;
    }
    else if (r->mode == 1) {
      ignored = ignored || m;
    }
    else {
      kept = kept || m;
    }
  }
  if (has_allow) {
    return allowed;
  }
  return !ignored || kept;
}

/// A descriptor of the at-rule, parsed by the registry's grammar. The declaration is dropped with a diagnostic when it is unknown or invalid.
static bool read_descriptor(ctx * c, const char * at_name, const GTEXT_CSS_Declaration * d, bool also_property) {
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &c->a->alloc;
  po.quirks = c->opt.quirks;
  char at[64];
  const size_t atn = strlen(at_name);
  if (atn + 2 > sizeof at) {
    return true;
  }
  at[0] = '@';
  memcpy(at + 1, at_name, atn + 1);
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  const GTEXT_CSS_At_Rule_Id aid = atn ? gtext_css_registry_find_at_rule(at, atn + 1) : 0;
  const bool custom = d->name.length > 2 && d->name.data[0] == '-' && d->name.data[1] == '-';
  GTEXT_CSS_Descriptor_Id did = aid && !custom ? gtext_css_registry_find_descriptor(aid, d->name.data, d->name.length) : 0;
  po.at_rule = did ? at : NULL;
  const GTEXT_CSS_Status st = gtext_css_parse_property_declaration(c->registry, d, &po, &pv);
  if (st == GTEXT_CSS_E_UNSUPPORTED) {
    gtext_css_property_value_free(&pv);
    return diag(c, GTEXT_CSS_ATRULE_DIAG_INVALID_VALUE, d->span, "the library cannot judge this declaration: it is dropped");
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_property_value_free(&pv);
    fail(c, st, &d->span, NULL);
    return false;
  }
  const bool unknown = pv.outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN;
  if (!did && !also_property) {
    gtext_css_property_value_free(&pv);
    return diag(c, GTEXT_CSS_ATRULE_DIAG_UNKNOWN_DESCRIPTOR, d->span, "Fonts 4, 4.3: \"declarations of any descriptors that are not supported by the user agent must be ignored\"");
  }
  if (unknown) {
    gtext_css_property_value_free(&pv);
    return diag(c, did ? GTEXT_CSS_ATRULE_DIAG_UNKNOWN_DESCRIPTOR : GTEXT_CSS_ATRULE_DIAG_UNKNOWN_PROPERTY, d->span, "CSS Syntax 5.5.6: an unknown name is dropped");
  }
  const bool wide_ok = !did; /* a descriptor takes no CSS-wide keyword and no substitution */
  if (pv.outcome == GTEXT_CSS_PROP_RESULT_INVALID || (!wide_ok && (pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING)) ||
      (did && d->important)) {
    gtext_css_property_value_free(&pv);
    return diag(c, did ? GTEXT_CSS_ATRULE_DIAG_INVALID_DESCRIPTOR : GTEXT_CSS_ATRULE_DIAG_INVALID_VALUE, d->span,
        did ? "Fonts 4, 4.3 (and the other descriptor rules): a descriptor whose value is not valid for it is ignored, and a descriptor takes no !important" : "CSS Cascading 4: an invalid declaration is dropped");
  }
  atrule_desc ad;
  memset(&ad, 0, sizeof ad);
  ad.pv = pv;
  ad.desc = did;
  ad.property = !did;
  ad.prop = did ? 0 : pv.property;
  ad.important = d->important;
  ad.decl = d->id;
  ad.span = d->span;
  if (custom) {
    ad.name = css_arena_strdup(&c->a->arena, d->name.data, d->name.length);
    ad.name_len = d->name.length;
  }
  else {
    ad.name = css_arena_strdup(&c->a->arena, d->name.data, d->name.length);
    ad.name_len = d->name.length;
    if (ad.name) {
      for (size_t i = 0; i < ad.name_len; i++) {
        if (ad.name[i] >= 'A' && ad.name[i] <= 'Z') {
          ad.name[i] = (char)(ad.name[i] + 32);
        }
      }
    }
  }
  if (!ad.name) {
    gtext_css_property_value_free(&ad.pv);
    fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    return false;
  }
  /* the value as canonical text: a typed value as the registry writes it, a custom property's tokens as written */
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &c->a->alloc;
  char * buf = NULL;
  size_t bl = 0;
  GTEXT_CSS_Status ss;
  if (ad.pv.set || ad.pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    ss = gtext_css_property_value_serialize_buffer(&ad.pv, &so, &buf, &bl);
    if (ss != GTEXT_CSS_OK) {
      gtext_css_property_value_free(&ad.pv);
      fail(c, ss, &d->span, NULL);
      return false;
    }
    ad.text = css_arena_strdup(&c->a->arena, buf, bl);
    gtext_css_serialize_buffer_free(&so, buf);
    ad.text_len = bl;
  }
  else {
    ad.text = text_in_arena(c, d->value, d->value_count, &ad.text_len);
  }
  if (!ad.text) {
    gtext_css_property_value_free(&ad.pv);
    if (c->st == GTEXT_CSS_OK) {
      fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    }
    return false;
  }
  return add_desc(c, &ad);
}

/* ---- the block ---------------------------------------------------------------------------------------------------------- */

typedef enum { BLOCK_DESCRIPTORS, BLOCK_PAGE, BLOCK_PROPERTIES, BLOCK_KEYFRAME, BLOCK_FEATURE, BLOCK_NONE } block_mode;

static bool add_kid(ctx * c, GTEXT_CSS_Atrule * kid) {
  GTEXT_CSS_Atrule * a = c->a;
  if (a->nkids >= c->opt.max_children) {
    gtext_css_atrule_free(kid);
    fail(c, GTEXT_CSS_E_LIMIT_DESCRIPTORS, &a->span, "max_children");
    return false;
  }
  if (!grow(&a->alloc, (void **)&a->kids, &a->capkids, a->nkids + 1, sizeof *a->kids)) {
    gtext_css_atrule_free(kid);
    fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    return false;
  }
  a->kids[a->nkids++] = kid;
  return true;
}

/// A feature block's declaration: `<custom-ident>: <integer [0,∞]>+` (the counts by block).
static bool read_feature_declaration(ctx * c, const GTEXT_CSS_Declaration * d) {
  const char * nm = c->a->name;
  size_t lo = 1, hi = 1;
  if (strcmp(nm, "styleset") == 0) {
    hi = 1 << 20;
  }
  else if (strcmp(nm, "character-variant") == 0) {
    hi = 2;
  }
  size_t count = 0;
  bool ok = d->name.length > 0 && !(d->name.length > 2 && d->name.data[0] == '-' && d->name.data[1] == '-' && false);
  for (size_t i = 0; i < d->value_count && ok; i++) {
    const GTEXT_CSS_Component_Value * v = &d->value[i];
    if (is_ws(v)) {
      continue;
    }
    if (v->kind != GTEXT_CSS_VALUE_TOKEN || v->type != GTEXT_CSS_TOKEN_NUMBER || !(v->flags & GTEXT_CSS_FLAG_INTEGER) || v->number < 0 || v->number > 2147483647.0) {
      ok = false;
    }
    count++;
  }
  if (!ok || count < lo || count > hi || d->important) {
    return diag(c, GTEXT_CSS_ATRULE_DIAG_INVALID_DESCRIPTOR, d->span, "Fonts 4, 6.4 (font-feature-values): a feature value declaration is <custom-ident>: <integer [0,∞]>+, with the number of values the feature takes");
  }
  atrule_desc ad;
  memset(&ad, 0, sizeof ad);
  ad.name = css_arena_strdup(&c->a->arena, d->name.data, d->name.length);
  ad.name_len = d->name.length;
  ad.decl = d->id;
  ad.span = d->span;
  ad.text = text_in_arena(c, d->value, d->value_count, &ad.text_len);
  if (!ad.name || !ad.text) {
    if (c->st == GTEXT_CSS_OK) {
      fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
    }
    return false;
  }
  return add_desc(c, &ad);
}

static GTEXT_CSS_Status parse_into(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Rule * rule, const GTEXT_CSS_Atrule_Options * options, bool keyframe_block,
    GTEXT_CSS_Atrule_Parent parent, GTEXT_CSS_Atrule ** out, GTEXT_CSS_Atrule_Result * result);

/// The nested rules and declarations of the rule's block.
static bool read_block(ctx * c, const GTEXT_CSS_Rule * rule, block_mode mode, const char * at_name) {
  GTEXT_CSS_Atrule * a = c->a;
  /* a qualified rule (a keyframe block) has its declarations and nested rules in their own fields; an at-rule has `contents` */
  const bool qualified = rule->kind == GTEXT_CSS_RULE_QUALIFIED;
  const size_t nitems = qualified ? 1 + rule->rule_count : rule->content_count;
  for (size_t it = 0; it < nitems; it++) {
    const GTEXT_CSS_Declaration * decls = NULL;
    size_t ndecl = 0;
    const GTEXT_CSS_Rule * sub = NULL;
    if (qualified) {
      if (it == 0) {
        decls = rule->declarations;
        ndecl = rule->declaration_count;
      }
      else {
        sub = &rule->rules[it - 1];
        if (sub->kind == GTEXT_CSS_RULE_NESTED_DECLARATIONS) {
          decls = sub->declarations;
          ndecl = sub->declaration_count;
          sub = NULL;
        }
      }
    }
    else {
      const GTEXT_CSS_Block_Item * bi = &rule->contents[it];
      if (bi->kind == GTEXT_CSS_ITEM_DECLARATIONS) {
        decls = bi->declarations;
        ndecl = bi->declaration_count;
      }
      else {
        sub = bi->rule;
      }
    }
    for (size_t i = 0; i < ndecl; i++) {
      const GTEXT_CSS_Declaration * d = &decls[i];
      bool ok = true;
      switch (mode) {
      case BLOCK_DESCRIPTORS:
        ok = read_descriptor(c, at_name, d, false);
        break;
      case BLOCK_PAGE:
        ok = read_descriptor(c, at_name, d, true);
        break;
      case BLOCK_FEATURE:
        ok = read_feature_declaration(c, d);
        break;
      case BLOCK_PROPERTIES:
      case BLOCK_KEYFRAME: {
        if (!property_taken(mode == BLOCK_KEYFRAME ? "keyframes" : at_name, d->name.data, d->name.length)) {
          ok = diag(c, GTEXT_CSS_ATRULE_DIAG_NOT_ALLOWED, d->span, mode == BLOCK_KEYFRAME ? "Animations 1, 3.1: the animation properties are ignored in a keyframe" :
                                                                   "Anchor Positioning 1, 6.4: @position-try only accepts the accepted @position-try properties");
        }
        else if (d->important) {
          ok = diag(c, mode == BLOCK_KEYFRAME ? GTEXT_CSS_ATRULE_DIAG_IMPORTANT_IGNORED : GTEXT_CSS_ATRULE_DIAG_NOT_ALLOWED, d->span,
              mode == BLOCK_KEYFRAME ? "Animations 1, 3.1: \"properties qualified with !important are invalid and ignored\" in a keyframe" :
                                       "Anchor Positioning 1, 6.4: \"It is invalid to use !important on the properties in the <declaration-list>\"");
        }
        else {
          ok = read_descriptor(c, "", d, true);
        }
        break;
      }
      case BLOCK_NONE:
        ok = diag(c, GTEXT_CSS_ATRULE_DIAG_NESTED_RULE, d->span, "this at-rule takes no declarations");
        break;
      }
      if (!ok) {
        return false;
      }
    }
    if (sub) {
      if (mode == BLOCK_PAGE || mode == BLOCK_FEATURE || (mode == BLOCK_DESCRIPTORS && a->kind == GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES)) {
        if (sub->kind != GTEXT_CSS_RULE_AT) {
          if (!diag(c, GTEXT_CSS_ATRULE_DIAG_NESTED_RULE, sub->span, "this at-rule takes only the at-rules of its grammar")) {
            return false;
          }
          continue;
        }
        GTEXT_CSS_Atrule * kid = NULL;
        GTEXT_CSS_Atrule_Result kr;
        memset(&kr, 0, sizeof kr);
        GTEXT_CSS_Atrule_Options ko = c->opt;
        const GTEXT_CSS_Status st = parse_into(c->registry, sub, &ko, false, a->kind == GTEXT_CSS_ATRULE_PAGE ? GTEXT_CSS_ATRULE_PARENT_PAGE : GTEXT_CSS_ATRULE_PARENT_FEATURE_VALUES, &kid, &kr);
        if (st != GTEXT_CSS_OK) {
          fail(c, st, &kr.span, kr.name);
          return false;
        }
        a->diag_total += kid ? kid->diag_total : 0;
        if (!kid) {
          if (!diag(c, GTEXT_CSS_ATRULE_DIAG_NESTED_RULE, sub->span, "an at-rule that is not valid here is ignored")) {
            return false;
          }
          continue;
        }
        if (!add_kid(c, kid)) {
          return false;
        }
      }
      else if (mode == BLOCK_KEYFRAME || mode == BLOCK_NONE || mode == BLOCK_DESCRIPTORS || mode == BLOCK_PROPERTIES) {
        if (!diag(c, GTEXT_CSS_ATRULE_DIAG_NESTED_RULE, sub->span, "this at-rule takes no nested rules")) {
          return false;
        }
      }
    }
  }
  return true;
}

/* ---- required descriptors ------------------------------------------------------------------------------------------------------ */

static size_t symbol_count(const GTEXT_CSS_Atrule * a, const char * name) {
  const int i = css_atrule_find_index(a, name, strlen(name));
  if (i < 0) {
    return 0;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(a->ds[i].pv.set);
  if (!root) {
    return 0;
  }
  return root->kind == GTEXT_CSS_VAL_LIST ? root->child_count : 1;
}

/// The value of `system` of a counter style (its first keyword), lower case; "symbolic" (the initial value) when it was not written.
static void counter_system(const GTEXT_CSS_Atrule * a, char * out, size_t cap) {
  css_copy_name(out, cap, "symbolic");
  const int i = css_atrule_find_index(a, "system", 6);
  if (i < 0) {
    return;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(a->ds[i].pv.set);
  if (root && root->kind == GTEXT_CSS_VAL_KEYWORD) {
    css_copy_name(out, cap, root->text.data);
  }
  else if (root && root->kind == GTEXT_CSS_VAL_LIST && root->child_count && root->children[0].kind == GTEXT_CSS_VAL_KEYWORD) {
    css_copy_name(out, cap, root->children[0].text.data);
  }
  for (char * p = out; *p; p++) {
    if (*p >= 'A' && *p <= 'Z') {
      *p = (char)(*p + 32);
    }
  }
}

static bool check_required(ctx * c) {
  GTEXT_CSS_Atrule * a = c->a;
  char system[32];
  system[0] = 0;
  if (a->kind == GTEXT_CSS_ATRULE_COUNTER_STYLE) {
    counter_system(a, system, sizeof system);
  }
  for (size_t i = 0; i < CSS_ATRULE_REQUIRED_COUNT; i++) {
    const css_atrule_required * r = &css_atrule_required_rows[i];
    if (strcmp(r->atrule, a->name) != 0) {
      continue;
    }
    if (r->system[0] && strcmp(r->system, system) != 0) {
      continue;
    }
    bool missing = false;
    for (size_t k = 0; k < 3 && r->descriptors[k]; k++) {
      const char * dn = r->descriptors[k];
      if (css_atrule_find_index(a, dn, strlen(dn)) < 0) {
        missing = true;
      }
      else if (r->minimum > 1 && k == 0 && symbol_count(a, dn) < r->minimum) {
        missing = true;
      }
    }
    if (!missing) {
      continue;
    }
    if (r->ignore_rule) {
      c->valid = false;
      return diag(c, GTEXT_CSS_ATRULE_DIAG_MISSING_REQUIRED, a->span, "the rule requires a descriptor that is missing: it is invalid and ignored");
    }
    a->defines_nothing = true;
    if (!diag(c, GTEXT_CSS_ATRULE_DIAG_NOT_DEFINED, a->span, "the rule is valid and defines nothing: a descriptor it requires is missing or too short")) {
      return false;
    }
  }
  return true;
}

/* ---- keyframe selectors -------------------------------------------------------------------------------------------------------- */

/// `<keyframe-selector># = [ from | to | <percentage [0,100]> ]#`.
static bool keyframe_selectors(ctx * c, const GTEXT_CSS_Component_Value * p, size_t n, bool * valid) {
  GTEXT_CSS_Atrule * a = c->a;
  size_t start = 0;
  *valid = true;
  for (;;) {
    size_t end = start;
    while (end < n && !is_comma(&p[end])) {
      end++;
    }
    const GTEXT_CSS_Component_Value * only = NULL;
    for (size_t i = start; i < end; i++) {
      if (is_ws(&p[i])) {
        continue;
      }
      if (only) {
        *valid = false;
        return true;
      }
      only = &p[i];
    }
    double off;
    if (!only || only->kind != GTEXT_CSS_VALUE_TOKEN) {
      *valid = false;
      return true;
    }
    if (only->type == GTEXT_CSS_TOKEN_IDENT && lower_eq(&only->value, "from")) {
      off = 0;
    }
    else if (only->type == GTEXT_CSS_TOKEN_IDENT && lower_eq(&only->value, "to")) {
      off = 1;
    }
    else if (only->type == GTEXT_CSS_TOKEN_PERCENTAGE && only->number >= 0 && only->number <= 100) {
      off = only->number / 100.0;
    }
    else {
      *valid = false;
      return true;
    }
    if (a->noffsets >= c->opt.max_list) {
      fail(c, GTEXT_CSS_E_LIMIT_DESCRIPTORS, &a->span, "max_list");
      return false;
    }
    size_t cap = a->noffsets;
    if (!grow(&a->alloc, (void **)&a->offsets, &cap, a->noffsets + 1, sizeof *a->offsets)) {
      fail(c, GTEXT_CSS_E_OOM, NULL, NULL);
      return false;
    }
    a->offsets[a->noffsets++] = off;
    if (end >= n) {
      break;
    }
    start = end + 1;
  }
  return true;
}

/* ---- the entry --------------------------------------------------------------------------------------------------------------- */

static void lower_in_place(char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] >= 'A' && s[i] <= 'Z') {
      s[i] = (char)(s[i] + 32);
    }
  }
}

static GTEXT_CSS_Status parse_into(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Rule * rule, const GTEXT_CSS_Atrule_Options * options, bool keyframe_block,
    GTEXT_CSS_Atrule_Parent parent, GTEXT_CSS_Atrule ** out, GTEXT_CSS_Atrule_Result * result) {
  *out = NULL;
  GTEXT_CSS_Atrule_Options o = options ? *options : gtext_css_atrule_options_default();
  if (!o.max_descriptors) {
    o.max_descriptors = DEF_DESCRIPTORS;
  }
  if (!o.max_children) {
    o.max_children = DEF_CHILDREN;
  }
  if (!o.max_diagnostics) {
    o.max_diagnostics = DEF_DIAGS;
  }
  if (!o.max_list) {
    o.max_list = DEF_LIST;
  }
  const GTEXT_Allocator alloc = o.allocator ? *o.allocator : *css_registry_allocator(registry);
  ctx cx;
  memset(&cx, 0, sizeof cx);
  cx.registry = registry;
  cx.opt = o;
  cx.opt.allocator = NULL;
  cx.rule = rule;
  cx.res = result;
  cx.valid = true;
  cx.max_diag = o.max_diagnostics;
  GTEXT_CSS_Atrule * a = gtext_allocator_calloc(&alloc, 1, sizeof *a);
  if (!a) {
    return fail(&cx, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  gcu_atomic_int_init(&a->refs, 1);
  a->alloc = alloc;
  css_arena_init(&a->arena, &a->alloc);
  a->registry = registry;
  a->span = rule->span;
  a->id = rule->id;
  cx.a = a;
  const GTEXT_CSS_Component_Value * p = rule->prelude;
  const size_t pn = rule->prelude_count;
  GTEXT_CSS_Atrule_Kind kind;
  const css_atrule_row * row = NULL;
  if (keyframe_block) {
    kind = GTEXT_CSS_ATRULE_KEYFRAME;
    a->name = "keyframe";
    a->name_len = 8;
  }
  else {
    char nm[64];
    size_t nl = rule->name.length < sizeof nm - 1 ? rule->name.length : sizeof nm - 1;
    memcpy(nm, rule->name.data, nl);
    nm[nl] = 0;
    lower_in_place(nm, nl);
    row = css_atrule_row_find(nm, rule->name.length);
    kind = row ? css_atrule_row_kind(row) : GTEXT_CSS_ATRULE_UNKNOWN;
    if (!row && parent == GTEXT_CSS_ATRULE_PARENT_FEATURE_VALUES) {
      /* the seven feature blocks of @font-feature-values are descriptors of it in the pin (`at-rule syntax`), not at-rules of their own */
      static const char * const features[] = {"stylistic", "historical-forms", "styleset", "character-variant", "swash", "ornaments", "annotation"};
      for (size_t i = 0; i < sizeof features / sizeof *features; i++) {
        if (strlen(features[i]) == nl && memcmp(features[i], nm, nl) == 0) {
          kind = GTEXT_CSS_ATRULE_FEATURE_BLOCK;
        }
      }
    }
    /* the compatibility alias of @keyframes is the same rule; the name the model keeps is the registry's */
    const char * canon = row ? row->name : NULL;
    if (row && strcmp(row->name, "-webkit-keyframes") == 0) {
      canon = "keyframes";
    }
    if (row && kind == GTEXT_CSS_ATRULE_PAGE_MARGIN) {
      canon = row->name;
    }
    a->name = css_arena_strdup(&a->arena, canon ? canon : nm, canon ? strlen(canon) : nl);
    a->name_len = canon ? strlen(canon) : nl;
    if (kind == GTEXT_CSS_ATRULE_PRESERVED || kind == GTEXT_CSS_ATRULE_UNKNOWN) {
      /* kept as written */
    }
  }
  if (!a->name) {
    atrule_destroy(a);
    return fail(&cx, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  a->kind = kind;
  a->max_children = o.max_children;
  a->max_descriptors = o.max_descriptors;
  /* the place */
  if (row) {
    static const uint8_t bit[] = {CSS_AR_P_TOP, CSS_AR_P_GROUP, CSS_AR_P_STYLE, CSS_AR_P_PAGE, CSS_AR_P_FEATURE_VALUES, CSS_AR_P_MIXIN};
    if (!(row->parents & bit[(int)parent])) {
      cx.valid = false;
      diag(&cx, GTEXT_CSS_ATRULE_DIAG_INVALID_RULE, rule->span, "the at-rule is not valid in this place");
    }
  }
  /* the prelude */
  bool prelude_ok = true;
  block_mode mode = BLOCK_NONE;
  const GTEXT_CSS_Component_Value * one = NULL;
  switch (kind) {
  case GTEXT_CSS_ATRULE_FONT_FACE:
  case GTEXT_CSS_ATRULE_VIEW_TRANSITION:
  case GTEXT_CSS_ATRULE_PAGE_MARGIN:
  case GTEXT_CSS_ATRULE_FEATURE_BLOCK:
  case GTEXT_CSS_ATRULE_STARTING_STYLE:
    prelude_ok = only_ws(p, pn);
    mode = kind == GTEXT_CSS_ATRULE_PAGE_MARGIN ? BLOCK_PROPERTIES : (kind == GTEXT_CSS_ATRULE_FEATURE_BLOCK ? BLOCK_FEATURE : BLOCK_DESCRIPTORS);
    break;
  case GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES: {
    bool v = true;
    prelude_ok = family_names(&cx, p, pn, &v) && v && cx.st == GTEXT_CSS_OK && pn > 0 && !only_ws(p, pn);
    mode = BLOCK_DESCRIPTORS;
    break;
  }
  case GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES:
  case GTEXT_CSS_ATRULE_POSITION_TRY:
    prelude_ok = single_ident(p, pn, &one) && one->value.length > 2 && one->value.data[0] == '-' && one->value.data[1] == '-';
    if (prelude_ok && !add_name(&cx, &one->value, false)) {
      prelude_ok = false;
    }
    mode = kind == GTEXT_CSS_ATRULE_POSITION_TRY ? BLOCK_PROPERTIES : BLOCK_DESCRIPTORS;
    break;
  case GTEXT_CSS_ATRULE_COLOR_PROFILE:
    prelude_ok = single_ident(p, pn, &one) && ((one->value.length > 2 && one->value.data[0] == '-' && one->value.data[1] == '-') || lower_eq(&one->value, "device-cmyk"));
    if (prelude_ok && !add_name(&cx, &one->value, false)) {
      prelude_ok = false;
    }
    mode = BLOCK_DESCRIPTORS;
    break;
  case GTEXT_CSS_ATRULE_COUNTER_STYLE:
    prelude_ok = single_ident(p, pn, &one) && counter_style_name_ok(&one->value);
    if (prelude_ok) {
      GTEXT_CSS_String nm = one->value;
      char * lc = NULL;
      if (predefined_counter_style(&nm)) {
        lc = css_arena_strdup(&a->arena, nm.data, nm.length);
        if (lc) {
          lower_in_place(lc, nm.length);
          nm.data = lc;
        }
      }
      prelude_ok = add_name(&cx, &nm, false);
    }
    mode = BLOCK_DESCRIPTORS;
    break;
  case GTEXT_CSS_ATRULE_KEYFRAMES: {
    const GTEXT_CSS_Component_Value * only = NULL;
    size_t sig = 0;
    for (size_t i = 0; i < pn; i++) {
      if (!is_ws(&p[i])) {
        only = &p[i];
        sig++;
      }
    }
    prelude_ok = sig == 1 && only->kind == GTEXT_CSS_VALUE_TOKEN &&
        ((only->type == GTEXT_CSS_TOKEN_IDENT && !lower_eq(&only->value, "none") && !wide_keyword_name(&only->value)) || only->type == GTEXT_CSS_TOKEN_STRING);
    if (prelude_ok && !add_name(&cx, &only->value, only->type == GTEXT_CSS_TOKEN_STRING)) {
      prelude_ok = false;
    }
    mode = BLOCK_NONE;
    break;
  }
  case GTEXT_CSS_ATRULE_KEYFRAME: {
    bool v = true;
    prelude_ok = keyframe_selectors(&cx, p, pn, &v) && v && cx.st == GTEXT_CSS_OK;
    mode = BLOCK_KEYFRAME;
    break;
  }
  case GTEXT_CSS_ATRULE_PAGE: {
    bool v = true;
    GTEXT_CSS_Page_Selector * sels = NULL;
    size_t ns = 0;
    const GTEXT_CSS_Status st = css_page_selectors_parse(&a->alloc, &a->arena, p, pn, o.max_list, &sels, &ns, &v);
    if (st != GTEXT_CSS_OK) {
      fail(&cx, st, &rule->prelude_span, NULL);
      prelude_ok = false;
    }
    else {
      prelude_ok = v;
      a->sels = sels;
      a->nsels = ns;
    }
    mode = BLOCK_PAGE;
    break;
  }
  case GTEXT_CSS_ATRULE_PROPERTY:
    mode = BLOCK_DESCRIPTORS;
    prelude_ok = !only_ws(p, pn);
    break;
  case GTEXT_CSS_ATRULE_NAMESPACE:
  case GTEXT_CSS_ATRULE_IMPORT:
  case GTEXT_CSS_ATRULE_LAYER:
  case GTEXT_CSS_ATRULE_SCOPE:
  case GTEXT_CSS_ATRULE_MEDIA:
  case GTEXT_CSS_ATRULE_SUPPORTS:
  case GTEXT_CSS_ATRULE_CONTAINER:
  case GTEXT_CSS_ATRULE_WHEN:
  case GTEXT_CSS_ATRULE_ELSE:
  case GTEXT_CSS_ATRULE_CUSTOM_MEDIA:
  case GTEXT_CSS_ATRULE_CHARSET:
  case GTEXT_CSS_ATRULE_PRESERVED:
  case GTEXT_CSS_ATRULE_UNKNOWN:
  default:
    mode = BLOCK_NONE;
    break;
  }
  if (cx.st != GTEXT_CSS_OK) {
    atrule_destroy(a);
    return cx.st;
  }
  /* the canonical text of the prelude */
  if (pn) {
    a->prelude = text_in_arena(&cx, p, pn, &a->prelude_len);
    if (!a->prelude) {
      atrule_destroy(a);
      return cx.st != GTEXT_CSS_OK ? cx.st : fail(&cx, GTEXT_CSS_E_OOM, NULL, NULL);
    }
  }
  if (!prelude_ok) {
    cx.valid = false;
    diag(&cx, GTEXT_CSS_ATRULE_DIAG_INVALID_RULE, rule->span, "the prelude does not match the at-rule's grammar: the rule is invalid and ignored");
  }
  if (cx.valid && cx.st == GTEXT_CSS_OK) {
    /* the block */
    const bool has_decls = kind == GTEXT_CSS_ATRULE_FONT_FACE || kind == GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES || kind == GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES ||
        kind == GTEXT_CSS_ATRULE_COLOR_PROFILE || kind == GTEXT_CSS_ATRULE_COUNTER_STYLE || kind == GTEXT_CSS_ATRULE_PAGE || kind == GTEXT_CSS_ATRULE_PAGE_MARGIN ||
        kind == GTEXT_CSS_ATRULE_FEATURE_BLOCK || kind == GTEXT_CSS_ATRULE_VIEW_TRANSITION || kind == GTEXT_CSS_ATRULE_POSITION_TRY || kind == GTEXT_CSS_ATRULE_KEYFRAME ||
        kind == GTEXT_CSS_ATRULE_PROPERTY;
    if (has_decls && (rule->has_block || rule->kind == GTEXT_CSS_RULE_QUALIFIED)) {
      read_block(&cx, rule, mode, keyframe_block ? "keyframe" : a->name);
    }
    else if (kind == GTEXT_CSS_ATRULE_KEYFRAMES && rule->has_block) {
      /* the blocks: qualified rules, each a keyframe */
      for (size_t it = 0; it < rule->content_count && cx.st == GTEXT_CSS_OK; it++) {
        const GTEXT_CSS_Block_Item * bi = &rule->contents[it];
        if (bi->kind != GTEXT_CSS_ITEM_RULE) {
          if (bi->declaration_count) {
            diag(&cx, GTEXT_CSS_ATRULE_DIAG_NESTED_RULE, bi->declarations[0].span, "a declaration directly in @keyframes is not a keyframe block: it is dropped");
          }
          continue;
        }
        if (bi->rule->kind != GTEXT_CSS_RULE_QUALIFIED) {
          diag(&cx, GTEXT_CSS_ATRULE_DIAG_NESTED_RULE, bi->rule->span, "an at-rule in @keyframes is not a keyframe block: it is dropped");
          continue;
        }
        GTEXT_CSS_Atrule * kid = NULL;
        GTEXT_CSS_Atrule_Result kr;
        memset(&kr, 0, sizeof kr);
        GTEXT_CSS_Atrule_Options ko = cx.opt;
        const GTEXT_CSS_Status st = parse_into(registry, bi->rule, &ko, true, GTEXT_CSS_ATRULE_PARENT_TOP, &kid, &kr);
        if (st != GTEXT_CSS_OK) {
          fail(&cx, st, &kr.span, kr.name);
          break;
        }
        if (!kid) {
          diag(&cx, GTEXT_CSS_ATRULE_DIAG_NESTED_RULE, bi->rule->span, "Animations 1, 3.1: a keyframe block whose selector is not valid is ignored");
          continue;
        }
        a->diag_total += kid->diag_total;
        if (!add_kid(&cx, kid)) {
          break;
        }
      }
    }
    if (cx.st == GTEXT_CSS_OK) {
      check_required(&cx);
    }
  }
  if (cx.st != GTEXT_CSS_OK) {
    atrule_destroy(a);
    return cx.st;
  }
  if (result) {
    result->valid = cx.valid;
    result->diagnostic_total = a->diag_total;
    result->diagnostic_count = a->ndiags;
  }
  if (!cx.valid) {
    /* an invalid rule is ignored whole: its diagnostics go with it (the result counts them) */
    atrule_destroy(a);
    return GTEXT_CSS_OK;
  }
  *out = a;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_atrule_parse(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Rule * rule, const GTEXT_CSS_Atrule_Options * options, GTEXT_CSS_Atrule ** out,
    GTEXT_CSS_Atrule_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!registry || !rule || !out || (rule->kind != GTEXT_CSS_RULE_AT)) {
    if (result) {
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Atrule_Parent parent = options ? options->parent : GTEXT_CSS_ATRULE_PARENT_TOP;
  const GTEXT_CSS_Status st = parse_into(registry, rule, options, false, parent, out, result);
  if (result && st != GTEXT_CSS_OK) {
    result->status = st;
    result->message = gtext_css_status_string(st);
  }
  return st;
}

/* ---- parsing one descriptor on its own ---------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_descriptor_parse(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Descriptor_Id descriptor, const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_Allocator * allocator, GTEXT_CSS_Value_Set ** set) {
  if (!registry || !set || (count && !values)) {
    return GTEXT_CSS_E_INVALID;
  }
  *set = NULL;
  GTEXT_CSS_Descriptor_Info info;
  if (!gtext_css_registry_descriptor_info(descriptor, &info) || info.kind != GTEXT_CSS_DESC_DECLARATION) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_At_Rule_Info ai;
  if (!gtext_css_registry_at_rule_info(info.at_rule, &ai)) {
    return GTEXT_CSS_E_INVALID;
  }
  char at[96];
  if (ai.name_length + 1 > sizeof at) {
    return GTEXT_CSS_E_INVALID;
  }
  memcpy(at, ai.name, ai.name_length);
  at[ai.name_length] = 0;
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = allocator;
  po.at_rule = at;
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  const GTEXT_CSS_Status st = gtext_css_parse_property(registry, info.name, info.name_length, values, count, &po, &pv);
  if (st != GTEXT_CSS_OK) {
    gtext_css_property_value_free(&pv);
    return st;
  }
  if (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID) {
    *set = pv.set;
    pv.set = NULL;
  }
  gtext_css_property_value_free(&pv);
  return GTEXT_CSS_OK;
}

/* ---- serialization (CSSOM: serialize a CSS rule) --------------------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_Markup_Sink * sink;
  GTEXT_CSS_Status st;
} outp;

static void put(outp * o, const char * s, size_t n) {
  if (o->st == GTEXT_CSS_OK && n && o->sink->write(o->sink->user, s, n) != GTEXT_MARKUP_OK) {
    o->st = GTEXT_CSS_E_WRITE;
  }
}

static void putz(outp * o, const char * s) {
  put(o, s, strlen(s));
}

static void put_ident(outp * o, const char * s, size_t n) {
  if (o->st == GTEXT_CSS_OK) {
    const GTEXT_CSS_Status st = gtext_css_serialize_identifier(s, n, o->sink);
    if (st != GTEXT_CSS_OK) {
      o->st = st;
    }
  }
}

static void put_string(outp * o, const char * s, size_t n) {
  if (o->st == GTEXT_CSS_OK) {
    const GTEXT_CSS_Status st = gtext_css_serialize_string(s, n, o->sink);
    if (st != GTEXT_CSS_OK) {
      o->st = st;
    }
  }
}

/// `name: value;` joined by a single space, in order ("serialize a CSS declaration block").
static void put_declarations(outp * o, const GTEXT_CSS_Atrule * a) {
  for (size_t i = 0; i < a->nds; i++) {
    if (i) {
      putz(o, " ");
    }
    put(o, a->ds[i].name, a->ds[i].name_len);
    putz(o, ": ");
    put(o, a->ds[i].text, a->ds[i].text_len);
    putz(o, a->ds[i].important ? " !important;" : ";");
  }
}

static void put_rule(outp * o, const GTEXT_CSS_Atrule * a, size_t indent);

static void put_names_prelude(outp * o, const GTEXT_CSS_Atrule * a) {
  for (size_t i = 0; i < a->nnames; i++) {
    if (i) {
      putz(o, ", ");
    }
    if (a->quoted[i]) {
      put_string(o, a->names[i].data, a->names[i].length);
    }
    else if (a->names[i].length > 2 && a->names[i].data[0] == '-' && a->names[i].data[1] == '-') {
      put_ident(o, a->names[i].data, a->names[i].length);
    }
    else {
      /* a family name of several idents is written as the idents */
      size_t s = 0;
      for (size_t k = 0; k <= a->names[i].length; k++) {
        if (k == a->names[i].length || a->names[i].data[k] == ' ') {
          if (k > s) {
            if (s) {
              putz(o, " ");
            }
            put_ident(o, a->names[i].data + s, k - s);
          }
          s = k + 1;
        }
      }
    }
  }
}

static void put_rule(outp * o, const GTEXT_CSS_Atrule * a, size_t indent) {
  switch (a->kind) {
  case GTEXT_CSS_ATRULE_KEYFRAMES: {
    putz(o, "@keyframes ");
    if (a->nnames) {
      const GTEXT_CSS_String * n = &a->names[0];
      if (a->quoted[0]) {
        put_string(o, n->data, n->length);
      }
      else {
        put_ident(o, n->data, n->length);
      }
    }
    putz(o, " {");
    for (size_t i = 0; i < a->nkids; i++) {
      putz(o, "\n  ");
      put_rule(o, a->kids[i], indent + 2);
    }
    putz(o, a->nkids ? "\n}" : " }");
    return;
  }
  case GTEXT_CSS_ATRULE_KEYFRAME: {
    for (size_t i = 0; i < a->noffsets; i++) {
      if (i) {
        putz(o, ", ");
      }
      char buf[40];
      /* offsets are percentages; from and to are written 0% and 100%, as keyText is */
      const double pc = a->offsets[i] * 100.0;
      size_t len = 0;
      const long long whole = (long long)pc;
      if ((double)whole == pc) {
        len = css_cssom_number(pc, true, buf);
      }
      else {
        len = css_cssom_number(pc, false, buf);
      }
      put(o, buf, len);
      putz(o, "%");
    }
    putz(o, " { ");
    put_declarations(o, a);
    putz(o, a->nds ? " }" : "}");
    return;
  }
  default:
    break;
  }
  putz(o, "@");
  put(o, a->name, a->name_len);
  if (a->prelude_len || a->nnames || a->nsels) {
    putz(o, " ");
    if (a->kind == GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES || a->kind == GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES || a->kind == GTEXT_CSS_ATRULE_COLOR_PROFILE ||
        a->kind == GTEXT_CSS_ATRULE_COUNTER_STYLE || a->kind == GTEXT_CSS_ATRULE_POSITION_TRY) {
      put_names_prelude(o, a);
    }
    else {
      put(o, a->prelude, a->prelude_len);
    }
  }
  if (a->kind == GTEXT_CSS_ATRULE_NAMESPACE || a->kind == GTEXT_CSS_ATRULE_IMPORT || a->kind == GTEXT_CSS_ATRULE_CHARSET || a->kind == GTEXT_CSS_ATRULE_CUSTOM_MEDIA) {
    putz(o, ";");
    return;
  }
  putz(o, " {");
  if (!a->nds && !a->nkids) {
    putz(o, " }");
    return;
  }
  if (!a->nkids) {
    putz(o, " ");
    put_declarations(o, a);
    putz(o, " }");
    return;
  }
  /* declarations and nested rules, each on a line of its own (CSSOM, a style rule that has rules) */
  if (a->nds) {
    putz(o, "\n  ");
    put_declarations(o, a);
  }
  for (size_t i = 0; i < a->nkids; i++) {
    putz(o, "\n  ");
    put_rule(o, a->kids[i], indent + 2);
  }
  putz(o, "\n}");
}

GTEXT_CSS_Status gtext_css_atrule_serialize(const GTEXT_CSS_Atrule * rule, const GTEXT_Markup_Sink * sink) {
  if (!rule || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  outp o = {sink, GTEXT_CSS_OK};
  put_rule(&o, rule, 0);
  return o.st;
}

typedef struct {
  const GTEXT_Allocator * alloc;
  char * data;
  size_t len, cap;
  bool failed;
} sbuf;

static GTEXT_Markup_Status sbuf_write(void * user, const char * bytes, size_t n) {
  sbuf * b = user;
  if (b->failed) {
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + n + 1) {
      nc *= 2;
    }
    char * nd = gtext_allocator_realloc(b->alloc, b->data, nc);
    if (!nd) {
      b->failed = true;
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = nd;
    b->cap = nc;
  }
  memcpy(b->data + b->len, bytes, n);
  b->len += n;
  b->data[b->len] = 0;
  return GTEXT_MARKUP_OK;
}

GTEXT_CSS_Status gtext_css_atrule_serialize_buffer(const GTEXT_CSS_Atrule * rule, const GTEXT_Allocator * allocator, char ** out, size_t * length) {
  if (!rule || !out || !length) {
    return GTEXT_CSS_E_INVALID;
  }
  sbuf b = {allocator ? allocator : gtext_allocator_default(), NULL, 0, 0, false};
  GTEXT_Markup_Sink sink = {sbuf_write, &b};
  GTEXT_CSS_Status st = gtext_css_atrule_serialize(rule, &sink);
  if (st == GTEXT_CSS_OK && !b.data) {
    b.data = gtext_allocator_malloc(b.alloc, 1);
    if (b.data) {
      b.data[0] = 0;
    }
    else {
      b.failed = true;
    }
  }
  if (b.failed) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(b.alloc, b.data);
    return st;
  }
  *out = b.data;
  *length = b.len;
  return GTEXT_CSS_OK;
}

void gtext_css_atrule_buffer_free(const GTEXT_Allocator * allocator, char * buffer) {
  gtext_allocator_free(allocator ? allocator : gtext_allocator_default(), buffer);
}

/* ---- the validity seam ---------------------------------------------------------------------------------------------------------------- */

static bool atrule_validity_rule(void * user, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Rule * rule) {
  GTEXT_CSS_Atrule_Validity * v = user;
  if (rule->kind != GTEXT_CSS_RULE_AT) {
    return true;
  }
  char nm[64];
  if (rule->name.length >= sizeof nm) {
    return true;
  }
  memcpy(nm, rule->name.data, rule->name.length);
  nm[rule->name.length] = 0;
  lower_in_place(nm, rule->name.length);
  const void * row = css_atrule_row_find(nm, rule->name.length);
  GTEXT_CSS_Atrule_Parent where = GTEXT_CSS_ATRULE_PARENT_TOP;
  if (!parent->top_level) {
    if (parent->kind == GTEXT_CSS_RULE_QUALIFIED) {
      where = GTEXT_CSS_ATRULE_PARENT_STYLE;
    }
    else if (css_ascii_ieq(&parent->name, "page")) {
      where = GTEXT_CSS_ATRULE_PARENT_PAGE;
    }
    else if (css_ascii_ieq(&parent->name, "font-feature-values")) {
      where = GTEXT_CSS_ATRULE_PARENT_FEATURE_VALUES;
    }
    else {
      where = GTEXT_CSS_ATRULE_PARENT_GROUP;
    }
  }
  if (!row && where != GTEXT_CSS_ATRULE_PARENT_FEATURE_VALUES) {
    return true;
  }
  GTEXT_CSS_Atrule_Options o = gtext_css_atrule_options_default();
  o.parent = where;
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Atrule_Result res;
  const GTEXT_CSS_Status st = gtext_css_atrule_parse(v->registry, rule, &o, &a, &res);
  gtext_css_atrule_free(a);
  v->checked++;
  /* a resource failure is not a verdict on the rule: keep it */
  if (st != GTEXT_CSS_OK || res.valid) {
    return true;
  }
  v->refused++;
  return false;
}

void gtext_css_atrule_validity_init(GTEXT_CSS_Atrule_Validity * v, const GTEXT_CSS_Registry * registry) {
  if (!v) {
    return;
  }
  memset(v, 0, sizeof *v);
  v->registry = registry;
  v->validity.user = v;
  v->validity.rule = atrule_validity_rule;
}
