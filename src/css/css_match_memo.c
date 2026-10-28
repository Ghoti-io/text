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
 * The memo of the matcher (css_match.h): an open-addressing table keyed by (parse, kind, step, element, scope), grown by doubling until the context's limit of entries.
 * An answer is stored once and read in O(1), which is what keeps the matching of many selectors against many elements bounded by (selector nodes) x elements.
 */

#include <string.h>

#include "css_match_internal.h"

#define MEMO_MIN_CAP ((size_t)64)

static size_t memo_hash(size_t uid, uint16_t kind, uint32_t step, const void * elem, const void * aux, const void * ts) {
  uint64_t h = 0x9e3779b97f4a7c15ull;
  h ^= (uint64_t)uid + 0x7f4a7c15u + (h << 6) + (h >> 2);
  h ^= (uint64_t)(uintptr_t)elem * 0xff51afd7ed558ccdull;
  h = (h << 31) | (h >> 33);
  h ^= (uint64_t)(uintptr_t)aux * 0xc4ceb9fe1a85ec53ull;
  h ^= ((uint64_t)step << 16 | kind) * 0x9e3779b97f4a7c15ull;
  h ^= (h << 13) + (uint64_t)(uintptr_t)ts * 0xd6e8feb86659fd93ull;
  h ^= h >> 29;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 32;
  return (size_t)h;
}

static bool same(const css_memo_ent * e, size_t uid, uint16_t kind, uint32_t step, const void * elem, const void * aux, const void * ts) {
  return e->uid == uid && e->kind == kind && e->step == step && e->elem == elem && e->aux == aux && e->ts == ts;
}

bool css_memo_get(struct GTEXT_CSS_Selector_Match_Context * ctx, size_t uid, uint16_t kind, uint32_t step, const void * elem, const void * aux, css_memo_ent * out) {
  if (ctx->opt.disable_memo || !ctx->cap) {
    return false;
  }
  size_t i = memo_hash(uid, kind, step, elem, aux, ctx->tree_scope) & (ctx->cap - 1);
  for (;;) {
    const css_memo_ent * e = &ctx->tab[i];
    if (e->val == 0) {
      return false;
    }
    if (same(e, uid, kind, step, elem, aux, ctx->tree_scope)) {
      *out = *e;
      ctx->ctr.memo_hits++;
      return true;
    }
    i = (i + 1) & (ctx->cap - 1);
  }
}

static bool grow(struct GTEXT_CSS_Selector_Match_Context * ctx) {
  const size_t ncap = ctx->cap ? ctx->cap * 2 : MEMO_MIN_CAP;
  if (ncap < ctx->cap || ncap > ((size_t)-1) / sizeof(css_memo_ent)) {
    return css_match_fail(ctx, GTEXT_CSS_E_OOM);
  }
  css_memo_ent * tab = gtext_allocator_calloc(&ctx->alloc, ncap, sizeof *tab);
  if (!tab) {
    return css_match_fail(ctx, GTEXT_CSS_E_OOM);
  }
  for (size_t k = 0; k < ctx->cap; k++) {
    const css_memo_ent * e = &ctx->tab[k];
    if (e->val == 0) {
      continue;
    }
    size_t i = memo_hash(e->uid, e->kind, e->step, e->elem, e->aux, e->ts) & (ncap - 1);
    while (tab[i].val != 0) {
      i = (i + 1) & (ncap - 1);
    }
    tab[i] = *e;
  }
  gtext_allocator_free(&ctx->alloc, ctx->tab);
  ctx->tab = tab;
  ctx->cap = ncap;
  return true;
}

bool css_memo_put(struct GTEXT_CSS_Selector_Match_Context * ctx, size_t uid, uint16_t kind, uint32_t step, const void * elem, const void * aux, uint16_t val, uint32_t a,
    uint32_t b) {
  if (ctx->opt.disable_memo) {
    return true;
  }
  if (!ctx->cap || (ctx->used + 1) * 2 > ctx->cap) {
    if (ctx->used + 1 > ctx->opt.max_memo_entries) {
      return css_match_fail(ctx, GTEXT_CSS_E_LIMIT_MATCH_MEMO);
    }
    if (!grow(ctx)) {
      return false;
    }
  }
  size_t i = memo_hash(uid, kind, step, elem, aux, ctx->tree_scope) & (ctx->cap - 1);
  for (;;) {
    css_memo_ent * e = &ctx->tab[i];
    if (e->val == 0) {
      if (ctx->used + 1 > ctx->opt.max_memo_entries) {
        return css_match_fail(ctx, GTEXT_CSS_E_LIMIT_MATCH_MEMO);
      }
      e->uid = uid;
      e->kind = kind;
      e->step = step;
      e->elem = elem;
      e->aux = aux;
      e->ts = ctx->tree_scope;
      e->val = val;
      e->a = a;
      e->b = b;
      ctx->used++;
      ctx->ctr.memo_entries = ctx->used;
      return true;
    }
    if (same(e, uid, kind, step, elem, aux, ctx->tree_scope)) {
      e->val = val;
      e->a = a;
      e->b = b;
      return true;
    }
    i = (i + 1) & (ctx->cap - 1);
  }
}

void css_memo_clear(struct GTEXT_CSS_Selector_Match_Context * ctx) {
  if (ctx->tab) {
    memset(ctx->tab, 0, ctx->cap * sizeof *ctx->tab);
  }
  ctx->used = 0;
  ctx->ctr.memo_entries = 0;
}
