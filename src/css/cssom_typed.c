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
 * `style` of a keyframe, a page rule, a margin rule, `@font-face`, `@position-try` and the other rules whose block the typed at-rule model reads (css_cssom_decl.h).
 *
 * The block is a view of the rule: the declarations that are properties are in a declaration block (so that a shorthand expands, `margin: auto` is four declarations and the
 * serialization is the one of every other block), and the descriptors that are not properties (`src`, `size`, `symbols`) are a short list after them. **The rule is the truth.**
 * Every mutation is made on a copy, the rule's own part is written as text from the copy, read again in the rule's place by the same parse a sheet is read with, and the mutation is
 * accepted only if the rule read from that text holds what was set (an `animation-name` in a keyframe, a `transform` that a page does not take, a `src` that is not a source list are
 * dropped by the typed model, and so refused here, with nothing changed). The view is then rebuilt from what the rule came to.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_typed.h"

bool css_typed_kind(GTEXT_CSSOM_Rule_Kind kind) {
  switch (kind) {
  case GTEXT_CSSOM_RULE_KEYFRAME:
  case GTEXT_CSSOM_RULE_PAGE:
  case GTEXT_CSSOM_RULE_PAGE_MARGIN:
  case GTEXT_CSSOM_RULE_FONT_FACE:
  case GTEXT_CSSOM_RULE_POSITION_TRY:
  case GTEXT_CSSOM_RULE_VIEW_TRANSITION:
  case GTEXT_CSSOM_RULE_COUNTER_STYLE:
  case GTEXT_CSSOM_RULE_FONT_PALETTE_VALUES:
  case GTEXT_CSSOM_RULE_COLOR_PROFILE:
    return true;
  default:
    return false;
  }
}

/* ---- the descriptor list ------------------------------------------------------------------------------------------------------------------ */

static void descs_free(css_core * core, css_desc * d, size_t n) {
  GTEXT_Allocator a = core->alloc;
  for (size_t i = 0; i < n; i++) {
    gtext_allocator_free(&a, d[i].name);
    gtext_allocator_free(&a, d[i].text);
  }
  gtext_allocator_free(&a, d);
}

void css_typed_free(GTEXT_CSSOM_Decl * decl) {
  descs_free(decl->obj.core, decl->descs, decl->ndescs);
  decl->descs = NULL;
  decl->ndescs = decl->capdescs = 0;
}

size_t css_typed_desc_count(const GTEXT_CSSOM_Decl * decl) {
  return decl->typed ? decl->ndescs : 0;
}

const css_desc * css_typed_desc_find(const GTEXT_CSSOM_Decl * decl, const char * name, size_t length) {
  for (size_t i = 0; decl->typed && i < decl->ndescs; i++) {
    if (decl->descs[i].name_length == length && memcmp(decl->descs[i].name, name, length) == 0) {
      return &decl->descs[i];
    }
  }
  return NULL;
}

static GTEXT_CSS_Status descs_push(css_core * core, css_desc ** list, size_t * n, size_t * cap, const char * name, size_t name_length, const char * text, size_t text_length,
    bool important) {
  if (*n == *cap) {
    const size_t ncap = *cap ? *cap * 2 : 4;
    GTEXT_Allocator a = core->alloc;
    css_desc * grown = gtext_allocator_realloc(&a, *list, ncap * sizeof **list);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    *list = grown;
    *cap = ncap;
  }
  css_desc * d = &(*list)[*n];
  d->name = css_core_dup(core, name, name_length);
  d->text = css_core_dup(core, text, text_length);
  if (!d->name || !d->text) {
    GTEXT_Allocator a = core->alloc;
    gtext_allocator_free(&a, d->name);
    gtext_allocator_free(&a, d->text);
    return GTEXT_CSS_E_OOM;
  }
  d->name_length = name_length;
  d->text_length = text_length;
  d->important = important;
  (*n)++;
  return GTEXT_CSS_OK;
}

/* ---- the view ----------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSS_Declaration_Block * block;
  css_desc * descs;
  size_t ndescs, capdescs;
} view;

static void view_free(css_core * core, view * v) {
  gtext_css_block_free(v->block);
  descs_free(core, v->descs, v->ndescs);
  memset(v, 0, sizeof *v);
}

/// The view of a typed model: its property declarations in a block, its descriptors in a list.
static GTEXT_CSS_Status view_from_typed(css_core * core, const GTEXT_CSS_Atrule * a, view * v) {
  memset(v, 0, sizeof *v);
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.allocator = &core->alloc;
  o.max_declarations = core->config.max_declarations;
  o.max_output_bytes = core->config.max_output_bytes;
  GTEXT_CSS_Status st = gtext_css_block_new(core->registry, &o, &v->block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &core->alloc;
  const size_t n = gtext_css_atrule_descriptor_count(a);
  if (n > core->config.max_work) {
    view_free(core, v);
    return GTEXT_CSS_E_LIMIT_WORK;
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < n; i++) {
    GTEXT_CSS_Atrule_Descriptor d;
    gtext_css_atrule_descriptor_at(a, i, &d);
    if (!d.property) {
      st = descs_push(core, &v->descs, &v->ndescs, &v->capdescs, d.name, d.name_length, d.text.data, d.text.length, d.important);
      continue;
    }
    GTEXT_CSS_Property_Outcome outcome;
    GTEXT_CSS_Shorthand_Result sr;
    const GTEXT_CSS_Status add = gtext_css_block_add_text(v->block, d.name, d.name_length, d.text.data, d.text.length, d.important, &po, &outcome, &sr);
    if (add == GTEXT_CSS_E_OOM || add == GTEXT_CSS_E_LIMIT_DECLARATIONS || add == GTEXT_CSS_E_LIMIT_WORK) {
      st = add;   /* anything else is a declaration the block cannot hold: it is not in the view */
    }
  }
  if (st != GTEXT_CSS_OK) {
    view_free(core, v);
  }
  return st;
}

static GTEXT_CSS_Status view_of_rule(css_rule * rule, view * v) {
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = css_rule_typed(rule->obj.core, rule, false, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = view_from_typed(rule->obj.core, a, v);
  gtext_css_atrule_free(a);
  return st;
}

GTEXT_CSS_Status css_typed_view(css_rule * rule, GTEXT_CSS_Declaration_Block ** block, css_desc ** descs, size_t * ndescs) {
  view v;
  GTEXT_CSS_Status st = view_of_rule(rule, &v);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *block = v.block;
  *descs = v.descs;
  *ndescs = v.ndescs;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_typed_refresh(css_rule * rule) {
  GTEXT_CSSOM_Decl * d = rule->style;
  if (!d || !d->typed) {
    return GTEXT_CSS_OK;
  }
  view v;
  GTEXT_CSS_Status st = view_of_rule(rule, &v);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  gtext_css_block_free(d->block);
  descs_free(rule->obj.core, d->descs, d->ndescs);
  d->block = v.block;
  d->descs = v.descs;
  d->ndescs = v.ndescs;
  d->capdescs = v.capdescs;
  return GTEXT_CSS_OK;
}

/* ---- names ---------------------------------------------------------------------------------------------------------------------------- */

static css_rule * decl_rule(const GTEXT_CSSOM_Decl * decl) {
  return (css_rule *)(uintptr_t)decl->owner;
}

bool css_typed_is_descriptor(const GTEXT_CSSOM_Decl * decl, const char * name, size_t length) {
  if (!decl->typed || !decl->owner || length == 0 || length > 48 || (name[0] == '-' && length > 1 && name[1] == '-')) {
    return false;
  }
  const css_rule * rule = decl_rule(decl);
  char at[64];
  const char * kind = css_kind((GTEXT_CSSOM_Rule_Kind)rule->kind)->name;
  const size_t kl = strlen(kind);
  if (kl + 2 > sizeof at) {
    return false;
  }
  at[0] = '@';
  memcpy(at + 1, kind, kl + 1);
  const GTEXT_CSS_At_Rule_Id id = gtext_css_registry_find_at_rule(at, kl + 1);
  if (!id) {
    return false;
  }
  char lower[48];
  for (size_t i = 0; i < length; i++) {
    lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
  }
  const GTEXT_CSS_Descriptor_Id did = gtext_css_registry_find_descriptor(id, lower, length);
  GTEXT_CSS_Descriptor_Info info;
  return did && gtext_css_registry_descriptor_info(did, &info) && info.kind == GTEXT_CSS_DESC_DECLARATION;
}

bool css_typed_idl_name(const char * attribute, size_t length, char * buf, size_t cap, size_t * n) {
  size_t o = 0;
  for (size_t i = 0; i < length; i++) {
    const char c = attribute[i];
    if (c >= 'A' && c <= 'Z') {
      if (o + 2 > cap) {
        return false;
      }
      buf[o++] = '-';
      buf[o++] = (char)(c + 32);
    }
    else {
      if (o + 1 > cap) {
        return false;
      }
      buf[o++] = c;
    }
  }
  *n = o;
  return true;
}

/* ---- candidate text ------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status css_typed_desc_text(const GTEXT_CSSOM_Decl * decl, tbuf * out) {
  for (size_t i = 0; i < decl->ndescs; i++) {
    const css_desc * d = &decl->descs[i];
    if ((out->len && !tb_puts(out, " ")) || !tb_put(out, d->name, d->name_length) || !tb_puts(out, ": ") || !tb_put(out, d->text, d->text_length) ||
        !tb_puts(out, d->important ? " !important;" : ";")) {
      return GTEXT_CSS_E_OOM;
    }
  }
  return GTEXT_CSS_OK;
}

/// The two ways a view differs from another one: the text of its block and its descriptors.
static bool same_view(css_core * core, GTEXT_CSS_Declaration_Block * a, const css_desc * da, size_t na, GTEXT_CSS_Declaration_Block * b, const css_desc * db, size_t nb, GTEXT_CSS_Status * st) {
  *st = GTEXT_CSS_OK;
  if (na != nb) {
    return false;
  }
  for (size_t i = 0; i < na; i++) {
    if (da[i].name_length != db[i].name_length || da[i].text_length != db[i].text_length || da[i].important != db[i].important || memcmp(da[i].name, db[i].name, da[i].name_length) ||
        memcmp(da[i].text, db[i].text, da[i].text_length)) {
      return false;
    }
  }
  tbuf x, y;
  tb_init(&x, &core->alloc);
  tb_init(&y, &core->alloc);
  *st = css_block_text(a, &x);
  if (*st == GTEXT_CSS_OK) {
    *st = css_block_text(b, &y);
  }
  const bool same = *st == GTEXT_CSS_OK && x.len == y.len && (x.len == 0 || memcmp(x.data, y.data, x.len) == 0);
  tb_free(&x);
  tb_free(&y);
  return same;
}

/// Read the rule again from `header { body }`: the candidate and its view. `*refused` when it is not a rule of this kind.
static GTEXT_CSS_Status read_candidate(css_rule * rule, const char * body, size_t body_length, css_own * own, view * v, bool * refused) {
  css_core * core = rule->obj.core;
  tbuf cand;
  tb_init(&cand, &core->alloc);
  GTEXT_CSS_Status st = css_rule_header_text(rule, &cand);
  if (st == GTEXT_CSS_OK && (!tb_puts(&cand, " { ") || !tb_put(&cand, body, body_length) || !tb_puts(&cand, " }") || cand.oom)) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    tb_free(&cand);
    return st;
  }
  st = css_own_prepare(rule, cand.data, cand.len, own, refused);
  tb_free(&cand);
  if (st != GTEXT_CSS_OK || *refused) {
    return st;
  }
  GTEXT_CSS_Atrule * a = NULL;
  st = css_own_typed(rule, own, false, &a);
  if (st == GTEXT_CSS_OK) {
    st = view_from_typed(core, a, v);
    gtext_css_atrule_free(a);
  }
  if (st != GTEXT_CSS_OK) {
    css_own_discard(own);
  }
  return st;
}

/// Everything about the change is decided: swap the rule's syntax and the view. Infallible.
static void install(GTEXT_CSSOM_Decl * decl, css_own * own, view * v) {
  css_core * core = decl->obj.core;
  css_own_swap(decl_rule(decl), own);
  gtext_css_block_free(decl->block);
  descs_free(core, decl->descs, decl->ndescs);
  decl->block = v->block;
  decl->descs = v->descs;
  decl->ndescs = v->ndescs;
  decl->capdescs = v->capdescs;
  memset(v, 0, sizeof *v);
}

GTEXT_CSS_Status css_typed_commit_block(GTEXT_CSSOM_Decl * decl, GTEXT_CSS_Declaration_Block * work, bool * accepted, GTEXT_CSSOM_Outcome * out) {
  (void)out;
  css_core * core = decl->obj.core;
  css_rule * rule = decl_rule(decl);
  *accepted = false;
  tbuf body;
  tb_init(&body, &core->alloc);
  GTEXT_CSS_Status st = css_block_text(work, &body);
  if (st == GTEXT_CSS_OK) {
    st = css_typed_desc_text(decl, &body);
  }
  if (st == GTEXT_CSS_OK && body.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  css_own own;
  view v;
  bool refused = false;
  memset(&v, 0, sizeof v);
  if (st == GTEXT_CSS_OK) {
    st = read_candidate(rule, body.data ? body.data : "", body.len, &own, &v, &refused);
  }
  tb_free(&body);
  if (st != GTEXT_CSS_OK || refused) {
    gtext_css_block_free(work);
    return st;
  }
  GTEXT_CSS_Status cmp = GTEXT_CSS_OK;
  /* the rule read again must hold exactly the declarations of the copy: a declaration it dropped is a declaration it refused */
  if (!same_view(core, work, decl->descs, decl->ndescs, v.block, v.descs, v.ndescs, &cmp) || cmp != GTEXT_CSS_OK) {
    view_free(core, &v);
    css_own_discard(&own);
    gtext_css_block_free(work);
    return cmp;
  }
  gtext_css_block_free(work);
  install(decl, &own, &v);
  *accepted = true;
  return GTEXT_CSS_OK;
}

static void ignore_value(GTEXT_CSSOM_Outcome * out, const char * detail) {
  if (out) {
    out->kind = GTEXT_CSSOM_IGNORED_INVALID_VALUE;
    out->detail = detail;
  }
}

GTEXT_CSS_Status css_typed_set_descriptor(GTEXT_CSSOM_Decl * decl, const char * name, size_t length, const char * value, size_t value_length, bool important, GTEXT_CSSOM_Outcome * out) {
  css_core * core = decl->obj.core;
  css_rule * rule = decl_rule(decl);
  char lower[48];
  for (size_t i = 0; i < length; i++) {
    lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
  }
  tbuf body;
  tb_init(&body, &core->alloc);
  GTEXT_CSS_Status st = css_block_text(decl->block, &body);
  bool placed = false;
  for (size_t i = 0; st == GTEXT_CSS_OK && i < decl->ndescs; i++) {
    const css_desc * d = &decl->descs[i];
    const bool same = d->name_length == length && memcmp(d->name, lower, length) == 0;
    placed = placed || same;
    if ((body.len && !tb_puts(&body, " ")) || !tb_put(&body, d->name, d->name_length) || !tb_puts(&body, ": ") || !tb_put(&body, same ? value : d->text, same ? value_length : d->text_length) ||
        !tb_puts(&body, (same ? important : d->important) ? " !important;" : ";")) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  if (st == GTEXT_CSS_OK && !placed &&
      ((body.len && !tb_puts(&body, " ")) || !tb_put(&body, lower, length) || !tb_puts(&body, ": ") || !tb_put(&body, value, value_length) || !tb_puts(&body, important ? " !important;" : ";"))) {
    st = GTEXT_CSS_E_OOM;
  }
  css_own own;
  view v;
  bool refused = false;
  memset(&v, 0, sizeof v);
  if (st == GTEXT_CSS_OK && body.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK) {
    st = read_candidate(rule, body.data ? body.data : "", body.len, &own, &v, &refused);
  }
  tb_free(&body);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const css_desc * now = NULL;
  for (size_t i = 0; !refused && i < v.ndescs; i++) {
    if (v.descs[i].name_length == length && memcmp(v.descs[i].name, lower, length) == 0) {
      now = &v.descs[i];
    }
  }
  if (refused || !now) {
    if (!refused) {
      view_free(core, &v);
      css_own_discard(&own);
    }
    ignore_value(out, "the value is not valid for the descriptor");
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Status cmp = GTEXT_CSS_OK;
  if (same_view(core, decl->block, decl->descs, decl->ndescs, v.block, v.descs, v.ndescs, &cmp) && cmp == GTEXT_CSS_OK) {
    view_free(core, &v);
    css_own_discard(&own);
    return GTEXT_CSS_OK;   /* what the rule already says */
  }
  if (cmp != GTEXT_CSS_OK) {
    view_free(core, &v);
    css_own_discard(&own);
    return cmp;
  }
  install(decl, &own, &v);
  css_decl_finish_change(decl, NULL, 0, NULL, 0, false, out);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_typed_remove_descriptor(GTEXT_CSSOM_Decl * decl, const char * name, size_t length, GTEXT_CSSOM_Outcome * out) {
  css_core * core = decl->obj.core;
  css_rule * rule = decl_rule(decl);
  char lower[48];
  for (size_t i = 0; i < length; i++) {
    lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
  }
  if (!css_typed_desc_find(decl, lower, length)) {
    return GTEXT_CSS_OK;
  }
  tbuf body;
  tb_init(&body, &core->alloc);
  GTEXT_CSS_Status st = css_block_text(decl->block, &body);
  for (size_t i = 0; st == GTEXT_CSS_OK && i < decl->ndescs; i++) {
    const css_desc * d = &decl->descs[i];
    if (d->name_length == length && memcmp(d->name, lower, length) == 0) {
      continue;
    }
    if ((body.len && !tb_puts(&body, " ")) || !tb_put(&body, d->name, d->name_length) || !tb_puts(&body, ": ") || !tb_put(&body, d->text, d->text_length) ||
        !tb_puts(&body, d->important ? " !important;" : ";")) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  css_own own;
  view v;
  bool refused = false;
  memset(&v, 0, sizeof v);
  if (st == GTEXT_CSS_OK && body.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK) {
    st = read_candidate(rule, body.data ? body.data : "", body.len, &own, &v, &refused);
  }
  tb_free(&body);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (refused) {
    return GTEXT_CSS_E_INTERNAL;   /* the rule was valid without it */
  }
  install(decl, &own, &v);
  css_decl_finish_change(decl, NULL, 0, NULL, 0, false, out);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_typed_set_css_text(GTEXT_CSSOM_Decl * decl, const char * text, size_t length, GTEXT_CSSOM_Outcome * out) {
  css_core * core = decl->obj.core;
  css_rule * rule = decl_rule(decl);
  css_own own;
  view v;
  bool refused = false;
  memset(&v, 0, sizeof v);
  GTEXT_CSS_Status st = read_candidate(rule, text, length, &own, &v, &refused);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (refused && length == 0) {
    return GTEXT_CSS_E_INTERNAL;
  }
  if (refused) {
    /* text that closes the block (`}`) or opens another rule is not a declaration list: the list is read as the parser reads a declaration list, and what is not one is empty */
    return css_typed_set_css_text(decl, "", 0, out);
  }
  GTEXT_CSS_Status cmp = GTEXT_CSS_OK;
  if (same_view(core, decl->block, decl->descs, decl->ndescs, v.block, v.descs, v.ndescs, &cmp) && cmp == GTEXT_CSS_OK) {
    view_free(core, &v);
    css_own_discard(&own);
    return GTEXT_CSS_OK;
  }
  if (cmp != GTEXT_CSS_OK) {
    view_free(core, &v);
    css_own_discard(&own);
    return cmp;
  }
  css_block_delta diff;
  memset(&diff, 0, sizeof diff);
  st = css_block_diff(decl->block, v.block, &diff);
  if (st != GTEXT_CSS_OK) {
    view_free(core, &v);
    css_own_discard(&own);
    return st;
  }
  GTEXT_CSS_Declaration_Block * old = decl->block;
  css_desc * old_descs = decl->descs;
  const size_t old_n = decl->ndescs;
  decl->block = NULL;
  decl->descs = NULL;
  decl->ndescs = 0;
  install(decl, &own, &v);
  css_decl_finish_change(decl, diff.ids, diff.id_count, diff.customs, diff.custom_count, false, out);
  css_block_diff_free(&diff);
  gtext_css_block_free(old);
  descs_free(core, old_descs, old_n);
  return GTEXT_CSS_OK;
}

/// Whether a property can be in the block of this rule at all (the pin's allow and ignore rows; a margin box takes any, a block of descriptors none).
static bool takes_property(const css_rule * rule, const char * name, size_t length) {
  switch ((GTEXT_CSSOM_Rule_Kind)rule->kind) {
  case GTEXT_CSSOM_RULE_KEYFRAME:
    return css_atrule_property_taken("keyframes", name, length);
  case GTEXT_CSSOM_RULE_PAGE:
    return css_atrule_property_taken("page", name, length);
  case GTEXT_CSSOM_RULE_POSITION_TRY:
    return css_atrule_property_taken("position-try", name, length);
  case GTEXT_CSSOM_RULE_PAGE_MARGIN:
    return true;
  default:
    return false;
  }
}

bool css_typed_has_attribute(const GTEXT_CSSOM_Decl * decl, const char * attribute, size_t length) {
  char dashed[64];
  size_t n = 0;
  if (!decl->typed || !decl->owner) {
    return false;
  }
  const css_rule * rule = decl_rule(decl);
  if (css_typed_idl_name(attribute, length, dashed, sizeof dashed, &n) && css_typed_is_descriptor(decl, dashed, n)) {
    return true;
  }
  const char * property = NULL;
  size_t pn = 0;
  if (!css_idl_lookup(attribute, length, &property, &pn)) {
    return false;
  }
  return takes_property(rule, property, pn);
}
