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
 * The `:nth-*` family of the matcher (css_match.h): the position of an element among its siblings, among those of its type, or among those that match a selector list
 * (`of S`). The positions of all the children of one parent are counted in one walk and kept in the memo, so that a node's evaluation costs O(siblings) once for each parent
 * and not once for each element: `nth_walks` counts the walks, and the quadratic suite asserts that it is the number of parents.
 */

#include <string.h>

#include "css_match_internal.h"

/// Positions of an element: from the start and from the end among the members, or not a member.
typedef struct {
  bool member;
  uint64_t first;
  uint64_t last;
} positions;

typedef struct {
  uint32_t hash;
  size_t nsoff, nslen, nameoff, namelen;
  uint32_t count;
  uint32_t seen;
} type_rec;

static uint32_t fnv(uint32_t h, const char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 16777619u;
  }
  return h;
}

static El prev_sibling(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  return ctx->iface.previous_sibling(ctx->iface.user, e);
}

static El next_sibling(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  return ctx->iface.next_sibling ? ctx->iface.next_sibling(ctx->iface.user, e) : NULL;
}

/// The element's type (namespace and local name) copied to `buf`.
static bool type_of(struct GTEXT_CSS_Selector_Match_Context * ctx, El e, char ** buf, size_t * len, size_t * cap, type_rec * t) {
  GTEXT_CSS_String ln = {NULL, 0}, ns = {NULL, 0};
  if (!(ctx->avail >> GTEXT_CSS_ELEMENT_CB_LOCAL_NAME & 1u) || !ctx->iface.local_name(ctx->iface.user, e, &ln) || !ln.data) {
    ln.data = "";
    ln.length = 0;
  }
  if ((ctx->avail >> GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI & 1u) && ctx->iface.namespace_uri(ctx->iface.user, e, &ns) && ns.data) {
    /* kept */
  }
  else {
    ns.data = "";
    ns.length = 0;
  }
  const size_t need = *len + ln.length + ns.length;
  if (need > *cap) {
    size_t ncap = *cap ? *cap : 256;
    while (ncap < need) {
      ncap *= 2;
    }
    char * nb = gtext_allocator_realloc(&ctx->alloc, *buf, ncap);
    if (!nb) {
      return css_match_fail(ctx, GTEXT_CSS_E_OOM);
    }
    *buf = nb;
    *cap = ncap;
  }
  t->nsoff = *len;
  t->nslen = ns.length;
  memcpy(*buf + *len, ns.data, ns.length);
  *len += ns.length;
  t->nameoff = *len;
  t->namelen = ln.length;
  memcpy(*buf + *len, ln.data, ln.length);
  *len += ln.length;
  t->hash = fnv(fnv(2166136261u, ns.data, ns.length), ln.data, ln.length);
  t->count = 0;
  t->seen = 0;
  return true;
}

/// Count the positions of every sibling of `e` in one walk and keep them. `mode`: 0 child, 1 of S, 2 of type.
static bool walk_siblings(struct GTEXT_CSS_Selector_Match_Context * ctx, El e, int mode, const GTEXT_CSS_Selector_List * s, uint16_t kind, size_t uid, uint32_t step,
    positions * want) {
  ctx->ctr.nth_walks++;
  // the siblings, first to last
  El first = e;
  for (El p = prev_sibling(ctx, e); p; p = prev_sibling(ctx, p)) {
    if (!css_match_work(ctx)) {
      return false;
    }
    first = p;
  }
  // local vectors: matching a selector of \`of S\` can walk the siblings of another parent, and must not touch these
  size_t n = 0, cap = 0;
  El * sib = NULL;
  uint8_t * mem = NULL;
  for (El x = first; x; x = next_sibling(ctx, x)) {
    if (!css_match_work(ctx)) {
      gtext_allocator_free(&ctx->alloc, sib);
      gtext_allocator_free(&ctx->alloc, mem);
      return false;
    }
    if (n == cap) {
      const size_t nc = cap ? cap * 2 : 64;
      El * np = gtext_allocator_realloc(&ctx->alloc, sib, nc * sizeof *np);
      if (np) {
        sib = np;
      }
      uint8_t * nv = np ? gtext_allocator_realloc(&ctx->alloc, mem, nc) : NULL;
      if (nv) {
        mem = nv;
      }
      if (!np || !nv) {
        gtext_allocator_free(&ctx->alloc, sib);
        gtext_allocator_free(&ctx->alloc, mem);
        return css_match_fail(ctx, GTEXT_CSS_E_OOM);
      }
      cap = nc;
    }
    sib[n++] = x;
  }
  // membership and, for a type, the type of each
  uint64_t total = 0;
  type_rec * types = NULL;
  uint32_t * tid = NULL;
  size_t ntypes = 0;
  char * nbuf = NULL;
  size_t nlen = 0, ncap = 0;
  bool ok = true;
  if (mode == 2) {
    types = gtext_allocator_calloc(&ctx->alloc, n ? n : 1, sizeof *types);
    tid = gtext_allocator_calloc(&ctx->alloc, n ? n : 1, sizeof *tid);
    if (!types || !tid) {
      ok = css_match_fail(ctx, GTEXT_CSS_E_OOM);
    }
  }
  for (size_t i = 0; ok && i < n; i++) {
    El x = sib[i];
    uint8_t m = 1;
    if (mode == 1) {
      m = css_match_any(ctx, s, x) ? 1 : 0;
      if (ctx->status != GTEXT_CSS_OK) {
        ok = false;
        break;
      }
    }
    mem[i] = m;
    if (!m) {
      continue;
    }
    total++;
    if (mode == 2) {
      type_rec t;
      if (!type_of(ctx, x, &nbuf, &nlen, &ncap, &t)) {
        ok = false;
        break;
      }
      size_t k = 0;
      for (; k < ntypes; k++) {
        if (types[k].hash == t.hash && types[k].nslen == t.nslen && types[k].namelen == t.namelen &&
            memcmp(nbuf + types[k].nsoff, nbuf + t.nsoff, t.nslen) == 0 && memcmp(nbuf + types[k].nameoff, nbuf + t.nameoff, t.namelen) == 0) {
          break;
        }
        if (!css_match_work(ctx)) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        break;
      }
      if (k == ntypes) {
        types[ntypes++] = t;
      }
      types[k].count++;
      tid[i] = (uint32_t)k;
    }
  }
  if (ok) {
    uint64_t pos = 0;
    for (size_t i = 0; i < n && ok; i++) {
      positions p;
      p.member = mem[i] != 0;
      p.first = p.last = 0;
      if (p.member) {
        if (mode == 2) {
          type_rec * t = &types[tid[i]];
          t->seen++;
          p.first = t->seen;
          p.last = (uint64_t)t->count - t->seen + 1;
        }
        else {
          pos++;
          p.first = pos;
          p.last = total - pos + 1;
        }
      }
      if (sib[i] == e) {
        *want = p;
      }
      if (!ctx->opt.disable_memo) {
        ok = css_memo_put(ctx, uid, kind, step, sib[i], NULL, p.member ? 3 : 1, (uint32_t)p.first, (uint32_t)p.last);
      }
    }
  }
  gtext_allocator_free(&ctx->alloc, sib);
  gtext_allocator_free(&ctx->alloc, mem);
  gtext_allocator_free(&ctx->alloc, types);
  gtext_allocator_free(&ctx->alloc, tid);
  gtext_allocator_free(&ctx->alloc, nbuf);
  return ok && ctx->status == GTEXT_CSS_OK;
}

/// The positions of `e` among its siblings (mode 0), the siblings that match `s` (1) or its type (2).
static bool positions_of(struct GTEXT_CSS_Selector_Match_Context * ctx, El e, int mode, const GTEXT_CSS_Selector_List * s, positions * out) {
  size_t uid = 0;
  uint32_t step = (uint32_t)mode;
  uint16_t kind = mode == 0 ? MK_NTH : MK_NTHF;
  if (mode == 1) {
    uid = s->uid;
    step = (uint32_t)s->id + 16u;
  }
  if (mode == 0 && (ctx->avail >> GTEXT_CSS_ELEMENT_CB_SIBLING_INDEX & 1u) && (ctx->avail >> GTEXT_CSS_ELEMENT_CB_SIBLING_COUNT & 1u)) {
    const size_t idx = ctx->iface.sibling_index(ctx->iface.user, e);
    const size_t cnt = ctx->iface.sibling_count(ctx->iface.user, e);
    if (idx >= 1 && cnt >= idx) {
      out->member = true;
      out->first = idx;
      out->last = (uint64_t)cnt - idx + 1;
      return true;
    }
  }
  css_memo_ent m;
  if (css_memo_get(ctx, uid, kind, step, e, NULL, &m)) {
    out->member = m.val == 3;
    out->first = m.a;
    out->last = m.b;
    return true;
  }
  return walk_siblings(ctx, e, mode, s, kind, uid, step, out);
}

/// Whether A n + B = pos has a solution n >= 0, exactly, whatever the sizes (magnitudes in unsigned 64 bits).
static bool anb_has(int64_t a, int64_t b, uint64_t pos) {
  // d = pos - b, as a sign and a magnitude
  bool dneg;
  uint64_t dmag;
  if (b >= 0) {
    const uint64_t bm = (uint64_t)b;
    dneg = pos < bm;
    dmag = dneg ? bm - pos : pos - bm;
  }
  else {
    dneg = false;
    dmag = pos + (uint64_t)(-(b + 1)) + 1u;
  }
  if (a == 0) {
    return dmag == 0;
  }
  const bool aneg = a < 0;
  const uint64_t am = aneg ? (uint64_t)(-(a + 1)) + 1u : (uint64_t)a;
  if (dmag == 0) {
    return true;
  }
  if (dneg != aneg) {
    return false;
  }
  return dmag % am == 0;
}

bool css_match_nth(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El element) {
  positions p;
  const sel_row * r = css_sel_row_of(s);
  const bool of_type = r->base[0] == 'n' && strstr(r->base, "of-type") != NULL;
  const bool from_end = strstr(r->base, "last") != NULL;
  const int mode = of_type ? 2 : s->has_of ? 1 : 0;
  if (!positions_of(ctx, element, mode, s->has_of ? s->list : NULL, &p)) {
    return false;
  }
  if (!p.member) {
    return false;
  }
  return anb_has(s->anb_a, s->anb_b, from_end ? p.last : p.first);
}

bool css_match_of_type(struct GTEXT_CSS_Selector_Match_Context * ctx, uint16_t row, El element) {
  positions p;
  if (!positions_of(ctx, element, 2, NULL, &p) || !p.member) {
    return false;
  }
  switch (row) {
  case GTEXT_CSS_SEL_PC_FIRST_OF_TYPE: return p.first == 1;
  case GTEXT_CSS_SEL_PC_LAST_OF_TYPE: return p.last == 1;
  default: return p.first == 1 && p.last == 1;
  }
}
