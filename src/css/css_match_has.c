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
 * `:has()` (css_match.h). A relative selector `:has(C1 c1 C2 ... Cm)` anchored at E is true if some element Y in the relation of its leading combinator to E satisfies the chain.
 * The chain below a point depends on the point and the step, and **not on E**, so `g(i, X)` ("X matches Ci and the rest of the chain exists from X") is memoized per
 * (relative selector, step, element), and so is the existence over descendants (`H`) or later siblings (`L`). Evaluating every anchor of a snapshot visits each (step,
 * element) once: O(m x n). The exception is a chain that mentions the anchor itself (`:scope` or `&`): the memo is then keyed by the anchor too, which is no sharing across
 * anchors, and the visits are spent from a per-call budget (`max_has_visits`, GTEXT_CSS_E_LIMIT_MATCH_WORK). No evaluation here recurses with the depth of the tree: the walk
 * below a point is an iteration with a stack of its own.
 */

#include <string.h>

#include "css_match_internal.h"

typedef struct {
  El node;
  El cur;
  bool acc;
} frame;

static bool visit(struct GTEXT_CSS_Selector_Match_Context * ctx) {
  ctx->ctr.has_visits++;
  if (ctx->has_dependent && ++ctx->has_count > ctx->opt.max_has_visits) {
    return css_match_fail(ctx, GTEXT_CSS_E_LIMIT_MATCH_WORK);
  }
  return css_match_work(ctx);
}

static El first_child(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  return ctx->iface.first_child(ctx->iface.user, e);
}

static El next_sibling(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  return ctx->iface.next_sibling(ctx->iface.user, e);
}

static bool ex(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * rc, size_t t, El x, const void * aux);

/// g(t, y): y matches the compound t and, if there is a compound after it, the chain exists from y.
static bool g(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * rc, size_t t, El y, const void * aux) {
  const GTEXT_CSS_Compound * cmp = &rc->compounds[t];
  css_memo_ent m;
  if (css_memo_get(ctx, list->uid, MK_G, (uint32_t)cmp->id, y, aux, &m)) {
    return m.val == 2;
  }
  if (!visit(ctx)) {
    return false;
  }
  bool ok = css_match_compound(ctx, cmp, y, cmp->simple_count);
  if (ok && t + 1 < rc->compound_count) {
    ok = ex(ctx, list, rc, t + 1, y, aux);
  }
  if (ctx->status != GTEXT_CSS_OK) {
    return false;
  }
  css_memo_put(ctx, list->uid, MK_G, (uint32_t)cmp->id, y, aux, ok ? 2 : 1, 0, 0);
  return ok && ctx->status == GTEXT_CSS_OK;
}

/// H(t, x): some proper descendant z of x has g(t, z). A post-order walk with a stack of its own; every node it finishes is kept.
static bool desc(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * rc, size_t t, El x, const void * aux) {
  const uint32_t step = (uint32_t)rc->compounds[t].id;
  css_memo_ent m;
  if (css_memo_get(ctx, list->uid, MK_H, step, x, aux, &m)) {
    return m.val == 2;
  }
  size_t top = 0, cap = 16;
  frame * st = gtext_allocator_malloc(&ctx->alloc, cap * sizeof *st);
  if (!st) {
    return css_match_fail(ctx, GTEXT_CSS_E_OOM);
  }
  st[0].node = x;
  st[0].cur = first_child(ctx, x);
  st[0].acc = false;
  bool result = false;
  bool ok = true;
  for (;;) {
    frame * f = &st[top];
    if (f->acc || !f->cur) {
      const bool val = f->acc;
      if (!css_memo_put(ctx, list->uid, MK_H, step, f->node, aux, val ? 2 : 1, 0, 0)) {
        ok = false;
        break;
      }
      if (top == 0) {
        result = val;
        break;
      }
      top--;
      st[top].acc = st[top].acc || val;
      continue;
    }
    El c = f->cur;
    f->cur = next_sibling(ctx, c);
    if (!visit(ctx)) {
      ok = false;
      break;
    }
    if (g(ctx, list, rc, t, c, aux)) {
      f->acc = true;
      continue;
    }
    if (ctx->status != GTEXT_CSS_OK) {
      ok = false;
      break;
    }
    if (css_memo_get(ctx, list->uid, MK_H, step, c, aux, &m)) {
      f->acc = f->acc || m.val == 2;
      continue;
    }
    if (top + 1 == cap) {
      cap *= 2;
      frame * ns = gtext_allocator_realloc(&ctx->alloc, st, cap * sizeof *st);
      if (!ns) {
        css_match_fail(ctx, GTEXT_CSS_E_OOM);
        ok = false;
        break;
      }
      st = ns;
    }
    top++;
    st[top].node = c;
    st[top].cur = first_child(ctx, c);
    st[top].acc = false;
  }
  gtext_allocator_free(&ctx->alloc, st);
  return ok && result && ctx->status == GTEXT_CSS_OK;
}

/// L(t, x): some later sibling y of x has g(t, y).
static bool later(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * rc, size_t t, El x, const void * aux) {
  const uint32_t step = (uint32_t)rc->compounds[t].id;
  css_memo_ent m;
  if (css_memo_get(ctx, list->uid, MK_L, step, x, aux, &m)) {
    return m.val == 2;
  }
  bool res = false;
  El stop = NULL;
  for (El y = next_sibling(ctx, x); y; y = next_sibling(ctx, y)) {
    if (!visit(ctx)) {
      return false;
    }
    if (g(ctx, list, rc, t, y, aux)) {
      res = true;
      stop = y;
      break;
    }
    if (ctx->status != GTEXT_CSS_OK) {
      return false;
    }
    if (css_memo_get(ctx, list->uid, MK_L, step, y, aux, &m)) {
      res = m.val == 2;
      stop = y;
      break;
    }
  }
  for (El y = x; y && y != stop; y = next_sibling(ctx, y)) {
    if (!css_memo_put(ctx, list->uid, MK_L, step, y, aux, res ? 2 : 1, 0, 0)) {
      return false;
    }
  }
  return res;
}

/// ex(t, x): some y related to x by the combinator before the compound t has g(t, y).
static bool ex(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * rc, size_t t, El x, const void * aux) {
  switch (rc->compounds[t].combinator) {
  case GTEXT_CSS_COMBINATOR_CHILD:
    for (El y = first_child(ctx, x); y; y = next_sibling(ctx, y)) {
      if (g(ctx, list, rc, t, y, aux)) {
        return true;
      }
      if (ctx->status != GTEXT_CSS_OK) {
        return false;
      }
    }
    return false;
  case GTEXT_CSS_COMBINATOR_NEXT_SIBLING: {
    El y = next_sibling(ctx, x);
    return y && g(ctx, list, rc, t, y, aux);
  }
  case GTEXT_CSS_COMBINATOR_DESCENDANT: return desc(ctx, list, rc, t, x, aux);
  case GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING: return later(ctx, list, rc, t, x, aux);
  default: return false;
  }
}

bool css_match_has(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El anchor) {
  const GTEXT_CSS_Selector_List * list = s->list;
  for (size_t i = 0; list && i < list->count; i++) {
    const GTEXT_CSS_Complex * rc = &list->complex[i];
    if (rc->never || rc->raw.data || !rc->relative) {
      continue;
    }
    const bool dep = rc->anchor_dependent;
    El save_scope = ctx->scope;
    const bool save_dep = ctx->has_dependent;
    const size_t save_count = ctx->has_count;
    const void * aux = NULL;
    if (dep) {
      // :scope inside :has() is the anchor, and nothing the chain says may be shared with another anchor
      ctx->scope = anchor;
      ctx->has_dependent = true;
      ctx->has_count = 0;
      aux = anchor;
    }
    const bool r = ex(ctx, list, rc, 0, anchor, aux);
    if (dep) {
      ctx->scope = save_scope;
      ctx->has_dependent = save_dep;
      ctx->has_count = save_count;
    }
    if (r) {
      return true;
    }
    if (ctx->status != GTEXT_CSS_OK) {
      return false;
    }
  }
  return false;
}
