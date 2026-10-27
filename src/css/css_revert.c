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
 * Deciding one property (css_cascade.h): the winner among its candidates, and the rollbacks `revert`, `revert-layer` and `revert-rule` (Cascade 5, 7.3.4 to
 * 7.3.6).
 *
 * The common case is one pass: the candidate with the greatest sort key. When that is a rolling keyword, the property's candidates are sorted once, in
 * descending order of precedence, and walked from the top: a rolled-back candidate is skipped, together with whatever the rollback says is rolled back, and
 * the walk ends at the first candidate that is neither. Each rollback only moves the walk forward, so a chain of them is linear in the number of candidates
 * (the sort being the one superlinear step, and only for the property that needs it). `all`, which is a candidate of every property, is sorted once for
 * the cascade and merged with the property's own, not copied into each.
 */

#include <string.h>

#include "css_cascade_internal.h"

/* ---- sorting ----------------------------------------------------------------------------------------------- */

void cascade_msort(uint32_t * a, uint32_t * tmp, size_t n, cascade_cmp cmp, const void * ctx, size_t * work) {
  for (size_t w = 1; w < n; w *= 2) {
    for (size_t i = 0; i < n; i += 2 * w) {
      const size_t mid = i + w < n ? i + w : n;
      const size_t hi = i + 2 * w < n ? i + 2 * w : n;
      size_t l = i, r = mid, o = i;
      while (l < mid && r < hi) {
        (*work)++;
        if (cmp(ctx, a[r], a[l]) < 0) {
          tmp[o++] = a[r++];
        }
        else {
          tmp[o++] = a[l++];
        }
      }
      while (l < mid) {
        tmp[o++] = a[l++];
      }
      while (r < hi) {
        tmp[o++] = a[r++];
      }
    }
    memcpy(a, tmp, n * sizeof *a);
  }
}

/// Stronger first.
static int by_precedence(const void * ctx, uint32_t x, uint32_t y) {
  const cascade_run * run = ctx;
  return cand_key_cmp(run->c[y].k, run->c[x].k);
}

static GTEXT_CSS_Status ensure_buffers(cascade_run * run, size_t n) {
  if (n <= run->buf_cap) {
    return GTEXT_CSS_OK;
  }
  size_t cap = run->buf_cap ? run->buf_cap : 16;
  while (cap < n) {
    cap *= 2;
  }
  uint32_t * a = gtext_allocator_realloc(&run->alloc, run->buf_a, cap * sizeof *a);
  if (!a) {
    return GTEXT_CSS_E_OOM;
  }
  run->buf_a = a;
  uint32_t * b = gtext_allocator_realloc(&run->alloc, run->buf_b, cap * sizeof *b);
  if (!b) {
    return GTEXT_CSS_E_OOM;
  }
  run->buf_b = b;
  run->buf_cap = cap;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status cascade_sorted(cascade_run * run, const uint32_t * own, size_t nown, bool use_all, uint32_t ** out, size_t * n) {
  const size_t total = nown + (use_all ? run->nall : 0);
  *out = NULL;
  *n = total;
  if (total == 0) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Status st = ensure_buffers(run, total);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  uint32_t * list = gtext_allocator_malloc(&run->alloc, total * sizeof *list);
  if (!list) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(list, own, nown * sizeof *list);
  if (use_all) {
    memcpy(list + nown, run->all, run->nall * sizeof *list);
  }
  cascade_msort(list, run->buf_b, total, by_precedence, run, &run->sort_work);
  *out = list;
  return GTEXT_CSS_OK;
}

/* ---- the walk ----------------------------------------------------------------------------------------------- */

/// Two sorted lists read as one, strongest first.
typedef struct {
  const cascade_run * run;
  const uint32_t * a;
  size_t na, ia;
  const uint32_t * b;
  size_t nb, ib;
} merged;

static uint32_t merged_next(merged * m, size_t * work) {
  if (m->ia < m->na && m->ib < m->nb) {
    (*work)++;
    return cand_key_cmp(m->run->c[m->a[m->ia]].k, m->run->c[m->b[m->ib]].k) > 0 ? m->a[m->ia++] : m->b[m->ib++];
  }
  if (m->ia < m->na) {
    return m->a[m->ia++];
  }
  if (m->ib < m->nb) {
    return m->b[m->ib++];
  }
  return CASCADE_NONE;
}

/* ---- what the walk has been told to skip --------------------------------------------------------------------- */

/// An entry of the skip table: a style rule that a `revert-rule` took out (kind 0: its origin, whether it is a hint or element-attached, its context, sheet and rule
/// -- not its importance: "as if the current style rule had not been present at all", and a rule's declarations are of both importances), or the normal
/// element-attached styles of an origin and context that an important element-attached `revert-layer` passed (kind 1).
typedef struct {
  uint32_t gen;
  uint32_t context, sheet, rule;
  uint8_t kind, origin, hint, attached;
} skip_ent;

static uint64_t skip_hash(const skip_ent * e) {
  uint64_t h = ((uint64_t)e->context << 32) ^ e->sheet;
  h ^= ((uint64_t)e->rule << 20) ^ ((uint64_t)e->kind << 4) ^ ((uint64_t)e->origin << 8) ^ ((uint64_t)e->hint << 12) ^ ((uint64_t)e->attached << 13);
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDULL;
  h ^= h >> 33;
  h *= 0xC4CEB9FE1A85EC53ULL;
  h ^= h >> 33;
  return h;
}

static void skip_key(skip_ent * e, int kind, const cand * c) {
  memset(e, 0, sizeof *e);
  e->kind = (uint8_t)kind;
  e->origin = c->origin;
  e->context = c->context;
  if (kind == 0) {
    e->hint = c->hint;
    e->attached = c->attached;
    e->sheet = c->sheet;
    e->rule = c->rule;
  }
}

static bool skip_same(const skip_ent * a, const skip_ent * b) {
  return a->kind == b->kind && a->origin == b->origin && a->context == b->context && a->hint == b->hint && a->attached == b->attached && a->sheet == b->sheet &&
      a->rule == b->rule;
}

static bool skip_has(cascade_run * run, int kind, const cand * c) {
  if (!run->skip_used) {
    return false;
  }
  skip_ent key;
  skip_key(&key, kind, c);
  const skip_ent * t = run->skip;
  size_t i = (size_t)(skip_hash(&key) & (run->skip_cap - 1));
  while (t[i].gen == run->skip_gen) {
    run->work++;
    if (skip_same(&t[i], &key)) {
      return true;
    }
    i = (i + 1) & (run->skip_cap - 1);
  }
  return false;
}

/// Add to the table; false when memory ran out.
static bool skip_add(cascade_run * run, int kind, const cand * c, size_t room) {
  if (!run->skip || (run->skip_used + 1) * 2 > run->skip_cap) {
    size_t cap = run->skip_cap ? run->skip_cap : 16;
    while (cap < 2 * (room + run->skip_used + 1)) {
      cap *= 2;
    }
    skip_ent * g = gtext_allocator_calloc(&run->alloc, cap, sizeof *g);
    if (!g) {
      return false;
    }
    /* what this walk has skipped so far moves over; older generations are dropped */
    const skip_ent * old = run->skip;
    for (size_t i = 0; old && i < run->skip_cap; i++) {
      if (old[i].gen == run->skip_gen) {
        size_t j = (size_t)(skip_hash(&old[i]) & (cap - 1));
        while (g[j].gen == run->skip_gen) {
          j = (j + 1) & (cap - 1);
        }
        g[j] = old[i];
      }
    }
    gtext_allocator_free(&run->alloc, run->skip);
    run->skip = g;
    run->skip_cap = cap;
  }
  skip_ent key;
  skip_key(&key, kind, c);
  key.gen = run->skip_gen;
  skip_ent * t = run->skip;
  size_t i = (size_t)(skip_hash(&key) & (run->skip_cap - 1));
  while (t[i].gen == run->skip_gen) {
    run->work++;
    if (skip_same(&t[i], &key)) {
      return true;
    }
    i = (i + 1) & (run->skip_cap - 1);
  }
  t[i] = key;
  run->skip_used++;
  return true;
}

GTEXT_CSS_Status cascade_resolve(cascade_run * run, const uint32_t * own, size_t nown, bool use_all, uint32_t * winner, uint32_t * rollbacks) {
  const cand * C = run->c;
  *rollbacks = 0;
  uint32_t best = CASCADE_NONE;
  for (size_t i = 0; i < nown; i++) {
    run->work++;
    if (best == CASCADE_NONE || cand_key_cmp(C[own[i]].k, C[best].k) > 0) {
      best = own[i];
    }
  }
  if (use_all) {
    for (size_t i = 0; i < run->nall; i++) {
      run->work++;
      if (best == CASCADE_NONE || cand_key_cmp(C[run->all[i]].k, C[best].k) > 0) {
        best = run->all[i];
      }
    }
  }
  if (best == CASCADE_NONE || !cand_rolls(&C[best])) {
    *winner = best;
    return GTEXT_CSS_OK;
  }
  /* the winner rolls back: sort what there is (the own candidates here, `all` once for the cascade) and walk */
  GTEXT_CSS_Status st = ensure_buffers(run, nown > run->nall ? nown : run->nall);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (use_all && !run->all_sorted && run->nall) {
    run->all_sorted = gtext_allocator_malloc(&run->alloc, run->nall * sizeof *run->all_sorted);
    if (!run->all_sorted) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(run->all_sorted, run->all, run->nall * sizeof *run->all_sorted);
    cascade_msort(run->all_sorted, run->buf_b, run->nall, by_precedence, run, &run->sort_work);
  }
  memcpy(run->buf_a, own, nown * sizeof *own);
  cascade_msort(run->buf_a, run->buf_b, nown, by_precedence, run, &run->sort_work);
  merged m;
  memset(&m, 0, sizeof m);
  m.run = run;
  m.a = run->buf_a;
  m.na = nown;
  m.b = use_all ? run->all_sorted : NULL;
  m.nb = use_all && run->all_sorted ? run->nall : 0;
  size_t *const work = &run->work;
  unsigned max_level = 2;
  bool skip_animation = false;
  /* a new generation: nothing the walks of other properties skipped is skipped here */
  run->skip_gen++;
  run->skip_used = 0;
  if (run->skip_gen == 0) {
    if (run->skip) {
      memset(run->skip, 0, run->skip_cap * sizeof(skip_ent));
    }
    run->skip_gen = 1;
  }
  const size_t room = nown + (use_all ? run->nall : 0);
  uint32_t x = merged_next(&m, work);
  while (x != CASCADE_NONE) {
    const cand * c = &C[x];
    (*work)++;
    if (c->level > max_level || (skip_animation && c->origin == GTEXT_CSS_ORIGIN_ANIMATION) ||
        (run->skip_used && (skip_has(run, 0, c) || (c->attached && !c->important && skip_has(run, 1, c))))) {
      x = merged_next(&m, work);
      continue;
    }
    if (!cand_rolls(c)) {
      *winner = x;
      return GTEXT_CSS_OK;
    }
    (*rollbacks)++;
    GTEXT_CSS_Wide_Keyword how = (GTEXT_CSS_Wide_Keyword)c->wide;
    if (how == GTEXT_CSS_WIDE_REVERT_RULE && (c->origin == GTEXT_CSS_ORIGIN_ANIMATION || c->origin == GTEXT_CSS_ORIGIN_TRANSITION)) {
      /* "The revert-rule keyword behaves like revert-layer in the animation origin." */
      how = GTEXT_CSS_WIDE_REVERT_LAYER;
    }
    switch (how) {
    case GTEXT_CSS_WIDE_REVERT:
      /* the cascaded value of the earlier origin: nothing of this origin or a later one is read; from the user-agent origin it is `unset` */
      if (c->level == 0) {
        *winner = CASCADE_NONE;
        return GTEXT_CSS_OK;
      }
      max_level = c->level - 1u;
      x = merged_next(&m, work);
      break;
    case GTEXT_CSS_WIDE_REVERT_LAYER:
      if (c->origin == GTEXT_CSS_ORIGIN_ANIMATION || c->origin == GTEXT_CSS_ORIGIN_TRANSITION) {
        /* "The animation origin ... effectively forms its own cascade layer." */
        do {
          x = merged_next(&m, work);
        } while (x != CASCADE_NONE && C[x].origin == c->origin);
      }
      else if (c->attached) {
        /* the rest of the element-attached styles of this importance and context; for important ones, "it only reverts the element-attached styles and the
           intervening animation origin": the normal element-attached styles of the context and the animation origin are skipped when the walk reaches them,
           and the important style rules between are not */
        do {
          x = merged_next(&m, work);
        } while (x != CASCADE_NONE && C[x].attached && C[x].k[CK_TIER] == c->k[CK_TIER] && C[x].context == c->context);
        if (c->important) {
          skip_animation = skip_animation || c->origin == GTEXT_CSS_ORIGIN_AUTHOR;
          if (!skip_add(run, 1, c, room)) {
            return GTEXT_CSS_E_OOM;
          }
        }
      }
      else {
        /* "as if no rules were specified in the current cascade layer, or between its normal and important levels in the cascade": everything from here down to
           the lowest key of the layer's normal level is skipped */
        uint32_t bound[CK_WORDS];
        memset(bound, 0, sizeof bound);
        bound[CK_TIER] = cand_normal_tier(c);
        bound[CK_CONTEXT] = UINT32_MAX - c->context;
        bound[CK_LAYER] = c->layer_normal;
        do {
          x = merged_next(&m, work);
        } while (x != CASCADE_NONE && cand_key_cmp(C[x].k, bound) >= 0);
      }
      break;
    case GTEXT_CSS_WIDE_REVERT_RULE:
      /* "as if the current style rule had not been present at all": every declaration of the rule, of either importance, wherever it sorts */
      if (!skip_add(run, 0, c, room)) {
        return GTEXT_CSS_E_OOM;
      }
      x = merged_next(&m, work);
      break;
    default:
      return GTEXT_CSS_E_INTERNAL;
    }
  }
  *winner = CASCADE_NONE;
  return GTEXT_CSS_OK;
}
