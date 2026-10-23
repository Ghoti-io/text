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
 * Matching component values against a linked grammar.
 *
 * ## The recognizer
 *
 * The question "does this grammar node, started at this position, match some
 * run of component values, and where can the run end?" has as its answer a set
 * of end positions, which is kept as a sorted list of intervals (so that
 * `<any-value>`, or a star over a run of identical tokens, is one interval and
 * not a position for each). That answer is memoized for every node that is not
 * a single terminal, keyed on (node, position); for `&&` and `||` the working
 * state is a set of (position, used-member mask) pairs, which is the third
 * component of the key the draft's backtracking needs.  A juxtaposition, an
 * alternation, a multiplier, a group, a function and a block each have a frame
 * on an explicit stack; when a frame needs the answer of another (node,
 * position) that is not known, the driver pushes a frame for it, so nothing
 * recurses and the depth of the stack is counted against the memo bound.
 * Left recursion was refused at link, so the frames a frame waits on never
 * include itself.
 *
 * A multiplier is evaluated by levels (the positions reachable in exactly k
 * repetitions), and once the count has passed its minimum and has no maximum
 * a position already seen is not expanded again, and a position this
 * multiplier was already worked from is not worked again: that is what makes a
 * star of a star, or of an alternation that can be empty, linear instead of
 * exponential. A multiplier that is an item of a juxtaposition is applied to the
 * whole set of positions at once, so `a* a* a* b` is linear as well, and the
 * positions of a set are visited from the highest to the lowest, so that the
 * answer for a position is built from those after it, which are already known.
 * What is not linear is an item applied one position at a time to the long run
 * of positions an enclosing repetition reaches (`[ a+ b? ]+` over n identical
 * tokens looks at the `b?` for each of up to n positions, n times over): the
 * work is then quadratic in the number of component values. It is finite, and
 * the work counter and its bound (`max_match_work`) are what answer for it.
 *
 * ## The typed value
 *
 * Once the whole input is known to match, the value is read off the memo again
 * with an explicit task stack: each node picks, among the ways its memoized
 * answers allow, the same one every time (alternatives in written order, the
 * earlier items of a juxtaposition as long as they can be, the first shortest
 * path through a repetition or a `&&`/`||` group), and builds the values the
 * css_value.h shape rules describe.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "css_grammar_internal.h"
#include "css_value_internal.h"

/* ---- limits and counters ---------------------------------------------- */

/// What a pending frame is worth in memo entries (a frame is about a hundred and fifty bytes).
#define FRAME_COST 4u

#define DEFAULT_MAX_MATCH_WORK ((size_t)67108864)
#define DEFAULT_MAX_MEMO_ENTRIES ((size_t)4194304)

#define NO_ITEM UINT32_MAX

GTEXT_CSS_Match_Options gtext_css_match_options_default(void) {
  GTEXT_CSS_Match_Options o;
  memset(&o, 0, sizeof o);
  o.max_match_work = DEFAULT_MAX_MATCH_WORK;
  o.max_memo_entries = DEFAULT_MAX_MEMO_ENTRIES;
  return o;
}

/* ---- the input, as the matcher sees it ---------------------------------- */

/// A component value of the input with white space left out, or the end of its view.
typedef struct {
  const GTEXT_CSS_Component_Value * cv; ///< NULL for the end of a view.
  uint32_t child;   ///< A function or block: the first item of the view of its contents.
  uint32_t end;     ///< The end item (sentinel) of the view this item is in.
  uint32_t orig;    ///< Index in the array of component values it came from.
  uint32_t cview;   ///< A function or block: the number of the view of its contents.
  uint32_t bad;     ///< First item at or after this one, in the view, that `<any-value>` refuses.
  uint32_t badd;    ///< The same for `<declaration-value>`, which also refuses a top-level `;` and `!`.
  bool ws_before;   ///< White space preceded it.
} mitem;

/// An interval of positions, inclusive.
typedef struct {
  uint32_t lo, hi;
} ival;

/// A set of positions, as sorted disjoint intervals that are not adjacent.
typedef struct {
  ival * v;
  size_t n, cap;
} iset;

/// An answer to "where can this node end", from the memo or computed on the spot.
typedef struct {
  const ival * v;
  size_t n;
  ival buf[6];
} ends_t;

typedef struct {
  uint32_t node, pos;
  uint32_t off;  ///< In the pool.
  uint32_t len;  ///< Intervals.
} mentry;

typedef enum {
  S_BEGIN = 0, S_LEVEL, S_EXPAND, S_AFTER_SET, S_CHILD
} fstate;

/// A frame: one node being worked at one position.
typedef struct {
  uint32_t node, pos;
  uint8_t state;
  uint8_t has_start;      ///< MULT: `a` was given (a set-level request).
  uint32_t i;             ///< A step, a kid or a level.
  uint32_t j;             ///< A member.
  uint32_t dest;          ///< A set-level frame: the frame that asked, +1; 0 for the memo.
  iset a, b, r, s;        ///< Working sets (see the steps).
  size_t cv;              ///< Cursor into `a`: the interval.
  uint32_t cp;            ///< Cursor into `a`: the position.
  bool cvalid;            ///< The cursor is on a position.
  struct { uint32_t pos; uint64_t mask; } * st; ///< && and ||: the states.
  size_t nst, stcap, qi;
  uint32_t * sh;          ///< Hash of state numbers + 1.
  size_t nsh;
} frame;

enum { GET_OK = 0, GET_BLOCK = 1, GET_ERR = 2 };
enum { ST_DONE = 0, ST_BLOCK = 1, ST_ERR = 2 };

typedef struct {
  const GTEXT_CSS_Compiled * c;
  GTEXT_Allocator alloc;
  size_t max_work, max_memo;
  size_t work, memo, memo_peak;
  GTEXT_CSS_Status status;
  bool unitless;
  size_t max_text;                  ///< Most bytes of the text of an opaque value (css_serialize.h's output limit).

  mitem * items;
  size_t nitems, icap;

  uint32_t * htab;
  size_t nslots;
  mentry * ents;
  size_t nents, ecap;
  ival * pool;
  size_t npool, pcap;

  frame * fr;
  size_t nfr, frcap;
  uint32_t need_node, need_pos;     ///< What a blocked step waits for.
  bool need_set;                    ///< ... a set-level multiplier instead of a (node, position).
  iset need_start;                  ///< ... with this start set (moved out of the asking frame).
  uint32_t need_dest;               ///< ... on behalf of this frame.
  iset * sync_out;                  ///< A set-level run made by the extraction: where its result goes.
} M;

static bool charge(M * m, size_t n) {
  m->work += n;
  if (m->work > m->max_work) {
    if (m->status == GTEXT_CSS_OK) {
      m->status = GTEXT_CSS_E_LIMIT_WORK;
    }
    return false;
  }
  return true;
}

static void fail_oom(M * m) {
  if (m->status == GTEXT_CSS_OK) {
    m->status = GTEXT_CSS_E_OOM;
  }
}

static bool memo_charge(M * m, size_t n) {
  if (n > m->max_memo - m->memo) {
    if (m->status == GTEXT_CSS_OK) {
      m->status = GTEXT_CSS_E_LIMIT_WORK;
    }
    return false;
  }
  m->memo += n;
  if (m->memo > m->memo_peak) {
    m->memo_peak = m->memo;
  }
  return true;
}

/* ---- interval sets ------------------------------------------------------ */

static void iset_free(M * m, iset * s) {
  gtext_allocator_free(&m->alloc, s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}

static bool iset_reserve(M * m, iset * s, size_t need) {
  void * v = s->v;
  if (!css_grammar_grow(&m->alloc, &v, &s->cap, need, sizeof(ival))) {
    fail_oom(m);
    return false;
  }
  s->v = v;
  return true;
}

/// Add positions lo..hi.
static bool iset_add_range(M * m, iset * s, uint32_t lo, uint32_t hi) {
  /* fast path: at or after the end */
  if (s->n > 0 && lo > s->v[s->n - 1].hi) {
    if (lo == s->v[s->n - 1].hi + 1) {
      s->v[s->n - 1].hi = hi;
      return true;
    }
  }
  if (s->n == 0 || lo > s->v[s->n - 1].hi) {
    if (!iset_reserve(m, s, s->n + 1)) {
      return false;
    }
    s->v[s->n].lo = lo;
    s->v[s->n].hi = hi;
    s->n++;
    return charge(m, 1);
  }
  /* general: the first interval whose end is at or after lo - 1 */
  size_t a = 0, b = s->n;
  while (a < b) {
    const size_t mid = (a + b) / 2;
    if ((uint64_t)s->v[mid].hi + 1 < lo) {
      a = mid + 1;
    }
    else {
      b = mid;
    }
  }
  size_t j = a;
  uint32_t nlo = lo, nhi = hi;
  while (j < s->n && (uint64_t)s->v[j].lo <= (uint64_t)hi + 1) {
    if (s->v[j].lo < nlo) {
      nlo = s->v[j].lo;
    }
    if (s->v[j].hi > nhi) {
      nhi = s->v[j].hi;
    }
    j++;
  }
  /* intervals a..j-1 are replaced by one */
  const size_t removed = j - a;
  if (removed == 0) {
    if (!iset_reserve(m, s, s->n + 1)) {
      return false;
    }
    memmove(&s->v[a + 1], &s->v[a], (s->n - a) * sizeof(ival));
    s->n++;
  }
  else if (removed > 1) {
    memmove(&s->v[a + 1], &s->v[j], (s->n - j) * sizeof(ival));
    s->n -= removed - 1;
  }
  s->v[a].lo = nlo;
  s->v[a].hi = nhi;
  return charge(m, 1 + (s->n - a) / 16);
}

static bool iset_add(M * m, iset * s, uint32_t p) {
  return iset_add_range(m, s, p, p);
}

static bool iset_has(const iset * s, uint32_t p) {
  size_t a = 0, b = s->n;
  while (a < b) {
    const size_t mid = (a + b) / 2;
    if (s->v[mid].hi < p) {
      a = mid + 1;
    }
    else if (s->v[mid].lo > p) {
      b = mid;
    }
    else {
      return true;
    }
  }
  return false;
}

static bool ends_has(const ends_t * e, uint32_t p) {
  for (size_t i = 0; i < e->n; i++) {
    if (e->v[i].lo <= p && p <= e->v[i].hi) {
      return true;
    }
  }
  return false;
}

static bool iset_union_v(M * m, iset * dst, const ival * v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!iset_add_range(m, dst, v[i].lo, v[i].hi)) {
      return false;
    }
  }
  return true;
}

/// dst = dst minus `sub`.
static bool iset_subtract(M * m, iset * dst, const iset * sub) {
  /* a merge over both lists */
  iset out = {NULL, 0, 0};
  size_t j = 0;
  for (size_t i = 0; i < dst->n; i++) {
    uint64_t lo = dst->v[i].lo;
    const uint64_t hi = dst->v[i].hi;
    while (j < sub->n && sub->v[j].hi < lo) {
      j++;
    }
    size_t k = j;
    bool alive = true;
    while (alive && k < sub->n && sub->v[k].lo <= hi) {
      if (sub->v[k].lo > lo) {
        if (!iset_reserve(m, &out, out.n + 1)) {
          iset_free(m, &out);
          return false;
        }
        out.v[out.n].lo = (uint32_t)lo;
        out.v[out.n].hi = sub->v[k].lo - 1;
        out.n++;
      }
      if ((uint64_t)sub->v[k].hi >= hi) {
        alive = false;
      }
      else {
        lo = (uint64_t)sub->v[k].hi + 1;
        k++;
      }
    }
    if (alive) {
      if (!iset_reserve(m, &out, out.n + 1)) {
        iset_free(m, &out);
        return false;
      }
      out.v[out.n].lo = (uint32_t)lo;
      out.v[out.n].hi = (uint32_t)hi;
      out.n++;
    }
    if (!charge(m, 1 + (k - j))) {
      iset_free(m, &out);
      return false;
    }
  }
  iset_free(m, dst);
  *dst = out;
  return true;
}

static bool iset_equal(const iset * a, const iset * b) {
  if (a->n != b->n) {
    return false;
  }
  return a->n == 0 || memcmp(a->v, b->v, a->n * sizeof(ival)) == 0;
}

/// A cursor over the positions of `a`, from the highest to the lowest. (An answer for a position is often
/// built from the answers of the positions after it, which are then already in the memo.)
static void cursor_start(frame * f) {
  f->cvalid = f->a.n > 0;
  if (f->cvalid) {
    f->cv = f->a.n - 1;
    f->cp = f->a.v[f->cv].hi;
  }
  else {
    f->cv = 0;
    f->cp = 0;
  }
}

static void cursor_next(frame * f) {
  if (f->cp > f->a.v[f->cv].lo) {
    f->cp--;
  }
  else if (f->cv > 0) {
    f->cv--;
    f->cp = f->a.v[f->cv].hi;
  }
  else {
    f->cvalid = false;
  }
}

/// Whether `a` is exactly one position.
static bool iset_single(const iset * s) {
  return s->n == 1 && s->v[0].lo == s->v[0].hi;
}

static void iset_swap(iset * x, iset * y) {
  const iset t = *x;
  *x = *y;
  *y = t;
}

/* ---- the memo ------------------------------------------------------------ */

static uint64_t memo_key(uint32_t node, uint32_t pos) {
  return ((uint64_t)node << 32) | pos;
}

static size_t memo_hash(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  return (size_t)k;
}

static bool memo_rehash(M * m) {
  const size_t nslots = m->nslots ? m->nslots * 2 : 1024;
  uint32_t * t = gtext_allocator_calloc(&m->alloc, nslots, sizeof *t);
  if (!t) {
    fail_oom(m);
    return false;
  }
  gtext_allocator_free(&m->alloc, m->htab);
  m->htab = t;
  m->nslots = nslots;
  for (size_t k = 0; k < m->nents; k++) {
    size_t i = memo_hash(memo_key(m->ents[k].node, m->ents[k].pos)) & (nslots - 1);
    while (t[i]) {
      i = (i + 1) & (nslots - 1);
    }
    t[i] = (uint32_t)(k + 1);
  }
  return true;
}

static const mentry * memo_find(const M * m, uint32_t node, uint32_t pos) {
  if (!m->nslots) {
    return NULL;
  }
  const size_t mask = m->nslots - 1;
  size_t i = memo_hash(memo_key(node, pos)) & mask;
  while (m->htab[i]) {
    const mentry * e = &m->ents[m->htab[i] - 1];
    if (e->node == node && e->pos == pos) {
      return e;
    }
    i = (i + 1) & mask;
  }
  return NULL;
}

/// `index` elements of `elem` bytes past `base`, where `base` may be NULL: then the answer is NULL. (`NULL + 0` is undefined
/// in C, and a memo whose entries all have no intervals, which is every memo of a grammar that matches nothing wide, has no
/// pool to be offset from.)
const void * css_ptr_at(const void * base, size_t index, size_t elem) {
  return base ? (const void *)((const char *)base + index * elem) : NULL;
}

static bool memo_put(M * m, uint32_t node, uint32_t pos, const iset * s) {
  if (!memo_charge(m, 1 + s->n)) {
    return false;
  }
  if ((m->nents + 1) * 2 > m->nslots && !memo_rehash(m)) {
    return false;
  }
  void * ents = m->ents;
  if (!css_grammar_grow(&m->alloc, &ents, &m->ecap, m->nents + 1, sizeof(mentry))) {
    fail_oom(m);
    return false;
  }
  m->ents = ents;
  void * pool = m->pool;
  /* (one more than the intervals: the pool is never NULL once there is an entry, whatever the entry holds) */
  if (!css_grammar_grow(&m->alloc, &pool, &m->pcap, m->npool + s->n + 1, sizeof(ival))) {
    fail_oom(m);
    return false;
  }
  m->pool = pool;
  mentry * e = &m->ents[m->nents];
  e->node = node;
  e->pos = pos;
  e->off = (uint32_t)m->npool;
  e->len = (uint32_t)s->n;
  if (s->n) {
    memcpy(m->pool + m->npool, s->v, s->n * sizeof(ival));
  }
  m->npool += s->n;
  m->nents++;
  const size_t mask = m->nslots - 1;
  size_t i = memo_hash(memo_key(node, pos)) & mask;
  while (m->htab[i]) {
    i = (i + 1) & mask;
  }
  m->htab[i] = (uint32_t)m->nents;
  return true;
}

/* ---- building the input's view ------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Component_Value * values;
  size_t count;
  uint32_t parent; ///< The item whose child view this is, or NO_ITEM.
  uint32_t start;  ///< Its first item, once built.
  uint32_t end;    ///< Its end item.
} pending_view;

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_bad_for_any(const GTEXT_CSS_Component_Value * v) {
  if (v->kind != GTEXT_CSS_VALUE_TOKEN) {
    return false;
  }
  switch (v->type) {
  case GTEXT_CSS_TOKEN_BAD_STRING: case GTEXT_CSS_TOKEN_BAD_URL: case GTEXT_CSS_TOKEN_CLOSE_PAREN:
  case GTEXT_CSS_TOKEN_CLOSE_SQUARE: case GTEXT_CSS_TOKEN_CLOSE_CURLY:
    return true;
  default:
    return false;
  }
}

static bool is_bad_for_decl(const GTEXT_CSS_Component_Value * v) {
  if (is_bad_for_any(v)) {
    return true;
  }
  return v->kind == GTEXT_CSS_VALUE_TOKEN
      && (v->type == GTEXT_CSS_TOKEN_SEMICOLON || (v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == '!'));
}

static bool build_items(M * m, const GTEXT_CSS_Component_Value * values, size_t count) {
  pending_view * q = NULL;
  size_t qn = 0, qcap = 0, qi = 0;
  void * qv = q;
  if (!css_grammar_grow(&m->alloc, &qv, &qcap, 1, sizeof *q)) {
    fail_oom(m);
    return false;
  }
  q = qv;
  q[qn++] = (pending_view){values, count, NO_ITEM, 0, 0};
  bool ok = true;
  while (ok && qi < qn) {
    const pending_view pv = q[qi++];
    if (m->nitems + pv.count + 1 >= UINT32_MAX - 1) {
      m->status = GTEXT_CSS_E_LIMIT_WORK;
      ok = false;
      break;
    }
    void * it = m->items;
    if (!css_grammar_grow(&m->alloc, &it, &m->icap, m->nitems + pv.count + 1, sizeof(mitem))) {
      fail_oom(m);
      ok = false;
      break;
    }
    m->items = it;
    const uint32_t start = (uint32_t)m->nitems;
    if (pv.parent != NO_ITEM) {
      m->items[pv.parent].child = start;
    }
    bool ws = false;
    for (size_t i = 0; i < pv.count && ok; i++) {
      const GTEXT_CSS_Component_Value * v = &pv.values[i];
      if (is_ws(v)) {
        ws = true;
        continue;
      }
      mitem * item = &m->items[m->nitems];
      memset(item, 0, sizeof *item);
      item->cv = v;
      item->child = NO_ITEM;
      item->orig = (uint32_t)i;
      item->ws_before = ws;
      ws = false;
      const uint32_t index = (uint32_t)m->nitems++;
      if ((v->kind == GTEXT_CSS_VALUE_FUNCTION || v->kind == GTEXT_CSS_VALUE_BLOCK)) {
        if (qn == qcap) {
          void * qq = q;
          if (!css_grammar_grow(&m->alloc, &qq, &qcap, qn + 1, sizeof *q)) {
            fail_oom(m);
            ok = false;
            break;
          }
          q = qq;
        }
        m->items[index].cview = (uint32_t)qn;
        q[qn++] = (pending_view){v->children, v->child_count, index, 0, 0};
      }
    }
    if (!ok) {
      break;
    }
    const uint32_t sentinel = (uint32_t)m->nitems;
    mitem * s = &m->items[m->nitems++];
    memset(s, 0, sizeof *s);
    s->child = NO_ITEM;
    s->orig = (uint32_t)pv.count;
    s->ws_before = ws;
    s->end = sentinel;
    s->bad = sentinel;
    s->badd = sentinel;
    for (uint32_t i = sentinel; i-- > start;) {
      m->items[i].end = sentinel;
    }
    q[qi - 1].start = start;
    q[qi - 1].end = sentinel;
    ok = ok && charge(m, pv.count + 1);
  }
  /* <any-value> and <declaration-value> match a sequence "that does not contain" a bad string, a bad url or an
     unmatched closing token (Variables 1, 2.1), and a function or a block contains what is inside it; the top-level
     restriction on a semicolon and on "!" is for the top level of the value only. So each item knows the first item at
     or after it in its view that its run may not reach, counting what is nested inside an item. The views are
     visited last to first, so that a view is done before the item that holds it. */
  uint8_t * nested = ok ? gtext_allocator_calloc(&m->alloc, qn, 1) : NULL;
  if (ok && !nested) {
    fail_oom(m);
    ok = false;
  }
  for (size_t v = qn; ok && v-- > 0;) {
    uint32_t nb = q[v].end, nd = q[v].end;
    bool any = false;
    for (uint32_t i = q[v].end; i-- > q[v].start;) {
      mitem * item = &m->items[i];
      const bool inside = (item->cv->kind != GTEXT_CSS_VALUE_TOKEN) && nested[item->cview];
      const bool own = is_bad_for_any(item->cv) || inside;
      any = any || own;
      if (own) {
        nb = i;
        nd = i;
      }
      if (is_bad_for_decl(item->cv)) {
        nd = i;
      }
      item->bad = nb;
      item->badd = nd;
    }
    nested[v] = any;
    ok = ok && charge(m, (size_t)(q[v].end - q[v].start) + 1);
  }
  gtext_allocator_free(&m->alloc, nested);
  gtext_allocator_free(&m->alloc, q);
  return ok;
}

/* ---- terminals ----------------------------------------------------------- */

static char fold1(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool fold_equal(const char * a, size_t an, const char * b, size_t bn) {
  if (an != bn) {
    return false;
  }
  for (size_t i = 0; i < an; i++) {
    if (fold1(a[i]) != fold1(b[i])) {
      return false;
    }
  }
  return true;
}

static bool is_math_function(const GTEXT_CSS_String * name) {
  static const char * const names[] = {"calc", "min", "max", "clamp", "round", "mod", "rem", "sin", "cos",
    "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp", "abs", "sign"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (fold_equal(name->data, name->length, names[i], strlen(names[i]))) {
      return true;
    }
  }
  return false;
}

static bool is_css_wide(const GTEXT_CSS_String * s) {
  static const char * const names[] = {"initial", "inherit", "unset", "revert", "revert-layer", "default"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (fold_equal(s->data, s->length, names[i], strlen(names[i]))) {
      return true;
    }
  }
  return false;
}

static bool tok(const GTEXT_CSS_Component_Value * v, GTEXT_CSS_Token_Type t) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == t;
}

/// Whether `v` (a number the range applies to) is inside `g`'s range.
static bool in_range(const gnode * g, double v) {
  if (!(g->flags & GNF_RANGE)) {
    return true;
  }
  return v >= g->lo && v <= g->hi;
}

/// Whether a quantity in unit `u` can be in `g`'s range. An absolute unit is converted and compared. A relative
/// unit (or one this library does not know) has no value until the environment is known, so it is in range when
/// some value of its sign is: its sign decides whether it can be (`-1em` is not in `[0px,∞]`, `1em` is in
/// `[0px,1in]`, and `0em` is not in `[1px,∞]`).
static bool unit_in_range(const gnode * g, GTEXT_CSS_Unit u, double n) {
  if (!(g->flags & GNF_RANGE)) {
    return true;
  }
  double c;
  if (gtext_css_unit_to_canonical(u, n, &c)) {
    return c >= g->lo && c <= g->hi;
  }
  if (n > 0) {
    return g->hi > 0;
  }
  if (n < 0) {
    return g->lo < 0;
  }
  return g->lo <= 0 && 0 <= g->hi;
}

static GTEXT_CSS_Unit_Type native_unit_type(gn_native t) {
  switch (t) {
  case NT_LENGTH: return GTEXT_CSS_UNIT_TYPE_LENGTH;
  case NT_ANGLE: return GTEXT_CSS_UNIT_TYPE_ANGLE;
  case NT_TIME: return GTEXT_CSS_UNIT_TYPE_TIME;
  case NT_FREQUENCY: return GTEXT_CSS_UNIT_TYPE_FREQUENCY;
  case NT_RESOLUTION: return GTEXT_CSS_UNIT_TYPE_RESOLUTION;
  case NT_FLEX: return GTEXT_CSS_UNIT_TYPE_FLEX;
  default: return GTEXT_CSS_UNIT_TYPE_NONE;
  }
}

static GTEXT_CSS_Unit unit_of(const GTEXT_CSS_Component_Value * v) {
  return gtext_css_unit_lookup(v->unit.data, v->unit.length);
}

/// A component of a <ratio>: <ratio> is "<number [0,infinity]> [ / <number [0,infinity]> ]?" (Values 4), so a number that is
/// not negative, or a math function, which stands wherever a <number> does.
static bool ratio_number(const GTEXT_CSS_Component_Value * v) {
  return (tok(v, GTEXT_CSS_TOKEN_NUMBER) && v->number >= 0) || (v->kind == GTEXT_CSS_VALUE_FUNCTION && is_math_function(&v->value));
}

/// A ratio's second part: `/` and a component, at items pos+1 and pos+2 (white space is not in the items).
static bool ratio_tail(const M * m, uint32_t pos) {
  const mitem * a = &m->items[pos + 1];
  if (!a->cv || !m->items[pos + 2].cv) {
    return false;
  }
  const mitem * b = &m->items[pos + 2];
  return a->cv->kind == GTEXT_CSS_VALUE_TOKEN && a->cv->type == GTEXT_CSS_TOKEN_DELIM && a->cv->delim == '/'
      && ratio_number(b->cv);
}

/// Whether native type `t` accepts the single component value at `pos` (not a ratio, an an+b or a run).
static bool native_one(const M * m, const gnode * g, uint32_t pos) {
  const mitem * it = &m->items[pos];
  const GTEXT_CSS_Component_Value * v = it->cv;
  const gn_native t = (gn_native)g->native;
  if (!v) {
    return false;
  }
  if (v->kind == GTEXT_CSS_VALUE_FUNCTION && css_grammar_native_numeric(t) && t != NT_DIMENSION) {
    return is_math_function(&v->value);
  }
  switch (t) {
  case NT_INTEGER:
    return tok(v, GTEXT_CSS_TOKEN_NUMBER) && (v->flags & GTEXT_CSS_FLAG_INTEGER) && in_range(g, v->number);
  case NT_NUMBER:
    return tok(v, GTEXT_CSS_TOKEN_NUMBER) && in_range(g, v->number);
  case NT_PERCENTAGE:
    return tok(v, GTEXT_CSS_TOKEN_PERCENTAGE) && in_range(g, v->number);
  case NT_DIMENSION:
    return tok(v, GTEXT_CSS_TOKEN_DIMENSION) && unit_in_range(g, unit_of(v), v->number);
  case NT_LENGTH:
    if (tok(v, GTEXT_CSS_TOKEN_NUMBER)) {
      if (v->number == 0) {
        return in_range(g, 0.0);
      }
      return m->unitless && in_range(g, v->number);
    }
    /* fall through */
  case NT_ANGLE: case NT_TIME: case NT_FREQUENCY: case NT_RESOLUTION: case NT_FLEX: {
    if (!tok(v, GTEXT_CSS_TOKEN_DIMENSION)) {
      return false;
    }
    const GTEXT_CSS_Unit u = unit_of(v);
    return u != GTEXT_CSS_UNIT_NONE && gtext_css_unit_type(u) == native_unit_type(t) && unit_in_range(g, u, v->number);
  }
  case NT_ZERO:
    return tok(v, GTEXT_CSS_TOKEN_NUMBER) && v->number == 0;
  case NT_STRING:
    return tok(v, GTEXT_CSS_TOKEN_STRING);
  case NT_URL: {
    if (tok(v, GTEXT_CSS_TOKEN_URL)) {
      return true;
    }
    if (v->kind != GTEXT_CSS_VALUE_FUNCTION || !css_ascii_ieq(&v->value, "url")) {
      return false;
    }
    size_t seen = 0;
    for (size_t i = 0; i < v->child_count; i++) {
      const GTEXT_CSS_Component_Value * c = &v->children[i];
      if (is_ws(c)) {
        continue;
      }
      if (seen == 0 ? !tok(c, GTEXT_CSS_TOKEN_STRING)
                    : !(tok(c, GTEXT_CSS_TOKEN_IDENT) || c->kind == GTEXT_CSS_VALUE_FUNCTION)) {
        return false;
      }
      seen++;
    }
    return seen > 0;
  }
  case NT_IDENT:
    return tok(v, GTEXT_CSS_TOKEN_IDENT);
  case NT_CUSTOM_IDENT:
    return tok(v, GTEXT_CSS_TOKEN_IDENT) && !is_css_wide(&v->value);
  case NT_DASHED_IDENT:
    return tok(v, GTEXT_CSS_TOKEN_IDENT) && v->value.length >= 2 && v->value.data[0] == '-' && v->value.data[1] == '-';
  case NT_URANGE:
    return tok(v, GTEXT_CSS_TOKEN_UNICODE_RANGE);
  case NT_TOK_IDENT: return tok(v, GTEXT_CSS_TOKEN_IDENT);
  case NT_TOK_FUNCTION: return v->kind == GTEXT_CSS_VALUE_FUNCTION;
  case NT_TOK_AT_KEYWORD: return tok(v, GTEXT_CSS_TOKEN_AT_KEYWORD);
  case NT_TOK_HASH: return tok(v, GTEXT_CSS_TOKEN_HASH);
  case NT_TOK_STRING: return tok(v, GTEXT_CSS_TOKEN_STRING);
  case NT_TOK_BAD_STRING: return tok(v, GTEXT_CSS_TOKEN_BAD_STRING);
  case NT_TOK_URL: return tok(v, GTEXT_CSS_TOKEN_URL);
  case NT_TOK_BAD_URL: return tok(v, GTEXT_CSS_TOKEN_BAD_URL);
  case NT_TOK_DELIM: return tok(v, GTEXT_CSS_TOKEN_DELIM);
  case NT_TOK_NUMBER: return tok(v, GTEXT_CSS_TOKEN_NUMBER);
  case NT_TOK_PERCENTAGE: return tok(v, GTEXT_CSS_TOKEN_PERCENTAGE);
  case NT_TOK_DIMENSION: return tok(v, GTEXT_CSS_TOKEN_DIMENSION);
  case NT_TOK_CDO: return tok(v, GTEXT_CSS_TOKEN_CDO);
  case NT_TOK_CDC: return tok(v, GTEXT_CSS_TOKEN_CDC);
  case NT_TOK_COLON: return tok(v, GTEXT_CSS_TOKEN_COLON);
  case NT_TOK_SEMICOLON: return tok(v, GTEXT_CSS_TOKEN_SEMICOLON);
  case NT_TOK_COMMA: return tok(v, GTEXT_CSS_TOKEN_COMMA);
  case NT_TOK_OPEN_SQUARE: return v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == GTEXT_CSS_TOKEN_OPEN_SQUARE;
  case NT_TOK_OPEN_PAREN: return v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == GTEXT_CSS_TOKEN_OPEN_PAREN;
  case NT_TOK_OPEN_CURLY: return v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == GTEXT_CSS_TOKEN_OPEN_CURLY;
  case NT_TOK_CLOSE_SQUARE: return tok(v, GTEXT_CSS_TOKEN_CLOSE_SQUARE);
  case NT_TOK_CLOSE_PAREN: return tok(v, GTEXT_CSS_TOKEN_CLOSE_PAREN);
  case NT_TOK_CLOSE_CURLY: return tok(v, GTEXT_CSS_TOKEN_CLOSE_CURLY);
  default:
    return false;
  }
}

static bool one_end(ends_t * e, uint32_t pos) {
  e->v = e->buf;
  e->buf[0].lo = e->buf[0].hi = pos;
  e->n = 1;
  return true;
}

/// The ends of a terminal node (a keyword, a literal or a native type) at `pos`.
static bool terminal_ends(M * m, uint32_t node, uint32_t pos, ends_t * e) {
  const gnode * g = &m->c->nodes[node];
  const mitem * it = &m->items[pos];
  const GTEXT_CSS_Component_Value * v = it->cv;
  e->v = e->buf;
  e->n = 0;
  if (!charge(m, 1)) {
    return false;
  }
  switch ((gn_kind)g->kind) {
  case GN_KEYWORD:
    if (v && tok(v, GTEXT_CSS_TOKEN_IDENT) && fold_equal(v->value.data, v->value.length, m->c->text + g->str_off, g->str_len)) {
      one_end(e, pos + 1);
    }
    return true;
  case GN_LITERAL: {
    if (!v || v->kind != GTEXT_CSS_VALUE_TOKEN) {
      return true;
    }
    bool hit;
    switch (g->lit) {
    case ',': hit = v->type == GTEXT_CSS_TOKEN_COMMA; break;
    case ':': hit = v->type == GTEXT_CSS_TOKEN_COLON; break;
    case ';': hit = v->type == GTEXT_CSS_TOKEN_SEMICOLON; break;
    default: hit = v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == g->lit; break;
    }
    if (hit) {
      one_end(e, pos + 1);
    }
    return true;
  }
  case GN_NATIVE: {
    const gn_native t = (gn_native)g->native;
    switch (t) {
    case NT_RATIO:
      if (v && ratio_number(v)) {
        one_end(e, pos + 1);
        if (ratio_tail(m, pos)) {
          e->buf[1].lo = e->buf[1].hi = pos + 3;
          e->n = 2;
        }
      }
      return true;
    case NT_ANY_VALUE: case NT_DECL_VALUE: {
      if (!v) {
        return true;
      }
      const uint32_t limit = t == NT_ANY_VALUE ? it->bad : it->badd;
      if (limit > pos) {
        e->buf[0].lo = pos + 1;
        e->buf[0].hi = limit;
        e->n = 1;
      }
      return true;
    }
    case NT_DECL_LIST: case NT_RULE_LIST:
      one_end(e, it->end);
      return true;
    case NT_TOK_WS:
      if (it->ws_before) {
        one_end(e, pos);
      }
      return true;
    case NT_ANB: {
      if (!v) {
        return true;
      }
      for (uint32_t k = 1; k <= 5 && pos + k - 1 < it->end; k++) {
        const mitem * last = &m->items[pos + k - 1];
        int64_t a, b;
        if (gtext_css_anb_parse(v, (size_t)(last->orig - it->orig + 1), &a, &b)) {
          if (e->n < sizeof e->buf / sizeof e->buf[0]) {
            e->buf[e->n].lo = e->buf[e->n].hi = pos + k;
            e->n++;
          }
        }
      }
      return true;
    }
    default:
      if (native_one(m, g, pos)) {
        one_end(e, pos + 1);
      }
      return true;
    }
  }
  default:
    return true;
  }
}

/* ---- asking for an answer --------------------------------------------- */

static bool is_terminal(const gnode * g) {
  return g->kind == GN_KEYWORD || g->kind == GN_LITERAL || g->kind == GN_NATIVE;
}

/// The ends of node `node` at `pos`: now (GET_OK), or after the frame for it has run (GET_BLOCK).
static int get_ends(M * m, uint32_t node, uint32_t pos, ends_t * e) {
  const gnode * g = &m->c->nodes[node];
  if (is_terminal(g)) {
    return terminal_ends(m, node, pos, e) ? GET_OK : GET_ERR;
  }
  const mentry * ent = memo_find(m, node, pos);
  if (ent) {
    e->v = (const ival *)css_ptr_at(m->pool, ent->off, sizeof(ival));
    e->n = ent->len;
    return GET_OK;
  }
  m->need_node = node;
  m->need_pos = pos;
  m->need_set = false;
  return GET_BLOCK;
}

#define ASK(node, pos, e) \
  do { \
    const int rc_ = get_ends(m, (node), (pos), (e)); \
    if (rc_ == GET_BLOCK) { \
      return ST_BLOCK; \
    } \
    if (rc_ == GET_ERR) { \
      return ST_ERR; \
    } \
  } while (0)

#define OKR(expr) \
  do { \
    if (!(expr)) { \
      return ST_ERR; \
    } \
  } while (0)

static bool is_closure(const gnode * g) {
  return g->kind == GN_MULT && !(g->flags & GNF_COMMA) && g->max == GN_INF && g->min <= 1;
}

/// A separator literal token at `pos`.
static bool literal_at(const M * m, uint32_t pos, uint32_t ch) {
  const GTEXT_CSS_Component_Value * v = m->items[pos].cv;
  if (!v || v->kind != GTEXT_CSS_VALUE_TOKEN) {
    return false;
  }
  switch (ch) {
  case ',': return v->type == GTEXT_CSS_TOKEN_COMMA;
  case ':': return v->type == GTEXT_CSS_TOKEN_COLON;
  case ';': return v->type == GTEXT_CSS_TOKEN_SEMICOLON;
  default: return v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == ch;
  }
}

static uint32_t kid_of(const M * m, const gnode * g, uint32_t i) {
  return m->c->kids[g->kids + i];
}

/// The separator character of a split SEQ.
static uint32_t sep_char(const gnode * g) {
  return g->sep == 1 ? ',' : '/';
}

/* ---- frames --------------------------------------------------------------- */

static void frame_free(M * m, frame * f) {
  iset_free(m, &f->a);
  iset_free(m, &f->b);
  iset_free(m, &f->r);
  iset_free(m, &f->s);
  gtext_allocator_free(&m->alloc, f->st);
  gtext_allocator_free(&m->alloc, f->sh);
  f->st = NULL;
  f->sh = NULL;
}

static frame * push_frame(M * m, uint32_t node, uint32_t pos) {
  if (!memo_charge(m, FRAME_COST) || !charge(m, 1)) {
    return NULL;
  }
  void * fr = m->fr;
  if (!css_grammar_grow(&m->alloc, &fr, &m->frcap, m->nfr + 1, sizeof(frame))) {
    fail_oom(m);
    return NULL;
  }
  m->fr = fr;
  frame * f = &m->fr[m->nfr++];
  memset(f, 0, sizeof *f);
  f->node = node;
  f->pos = pos;
  return f;
}

static void pop_frame(M * m) {
  frame_free(m, &m->fr[m->nfr - 1]);
  m->nfr--;
  m->memo -= FRAME_COST;
}

/// A frame is finished with the set `r`: remember it, or hand it to the frame that asked.
static int finish(M * m, frame * f) {
  if (f->dest == UINT32_MAX) {
    iset_swap(m->sync_out, &f->r);
    return ST_DONE;
  }
  if (f->dest) {
    frame * d = &m->fr[f->dest - 1];
    iset_swap(&d->b, &f->r);
    d->state = S_AFTER_SET;
    return ST_DONE;
  }
  return memo_put(m, f->node, f->pos, &f->r) ? ST_DONE : ST_ERR;
}

/* ---- the steps ------------------------------------------------------------ */

static int step_alt(M * m, frame * f) {
  const gnode * g = &m->c->nodes[f->node];
  ends_t e;
  while (f->i < g->nkids) {
    ASK(kid_of(m, g, f->i), f->pos, &e);
    OKR(iset_union_v(m, &f->r, e.v, e.n));
    f->i++;
  }
  return finish(m, f);
}

static int step_req(M * m, frame * f) {
  const gnode * g = &m->c->nodes[f->node];
  ends_t e;
  ASK(kid_of(m, g, 0), f->pos, &e);
  OKR(iset_union_v(m, &f->r, e.v, e.n));
  iset only = {NULL, 0, 0};
  const bool added = iset_add(m, &only, f->pos); /* (it may allocate and then fail at the work bound: freed either way) */
  const bool good = added && iset_subtract(m, &f->r, &only);
  iset_free(m, &only);
  OKR(good);
  return finish(m, f);
}

static int step_func(M * m, frame * f) {
  const gnode * g = &m->c->nodes[f->node];
  const mitem * it = &m->items[f->pos];
  const GTEXT_CSS_Component_Value * v = it->cv;
  if (f->state == S_BEGIN) {
    bool hit = false;
    if (v) {
      if (g->kind == GN_FUNC) {
        hit = v->kind == GTEXT_CSS_VALUE_FUNCTION
            && fold_equal(v->value.data, v->value.length, m->c->text + g->str_off, g->str_len);
      }
      else {
        hit = v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == (GTEXT_CSS_Token_Type)g->native;
      }
    }
    if (!hit) {
      return finish(m, f);
    }
    if (g->nkids == 0) {
      if (it->child == m->items[it->child].end) {
        OKR(iset_add(m, &f->r, f->pos + 1));
      }
      return finish(m, f);
    }
    f->state = S_CHILD;
  }
  ends_t e;
  ASK(kid_of(m, g, 0), it->child, &e);
  if (ends_has(&e, m->items[it->child].end)) {
    OKR(iset_add(m, &f->r, f->pos + 1));
  }
  return finish(m, f);
}

/// Steps of a SEQ: a plain one has a step for each kid; a split one a kid, a separator, a kid ...
static uint32_t seq_steps(const gnode * g) {
  return g->sep ? 2 * g->nkids - 1 : g->nkids;
}

static int step_seq(M * m, frame * f) {
  const gnode * g = &m->c->nodes[f->node];
  const uint32_t nsteps = seq_steps(g);
  ends_t e;
  if (f->state == S_BEGIN) {
    OKR(iset_add(m, &f->a, f->pos));
    f->i = 0;
    f->state = S_LEVEL;
  }
  for (;;) {
    if (f->state == S_AFTER_SET) {
      /* a set-level multiplier answered in b */
      iset_swap(&f->a, &f->b);
      f->b.n = 0;
      f->i++;
      f->state = S_LEVEL;
    }
    if (f->state == S_LEVEL) {
      if (f->a.n == 0 || f->i == nsteps) {
        iset_swap(&f->r, &f->a);
        return finish(m, f);
      }
      const bool is_sep = g->sep && (f->i % 2 == 1);
      if (!is_sep) {
        const uint32_t kid = kid_of(m, g, g->sep ? f->i / 2 : f->i);
        if (is_closure(&m->c->nodes[kid]) && !iset_single(&f->a)) {
          m->need_set = true;
          m->need_node = kid;
          m->need_dest = (uint32_t)(f - m->fr) + 1;
          m->need_start = f->a;
          memset(&f->a, 0, sizeof f->a);
          return ST_BLOCK;
        }
      }
      f->b.n = 0;
      cursor_start(f);
      f->state = S_EXPAND;
    }
    while (f->cvalid) {
      const uint32_t p = f->cp;
      if (g->sep && (f->i % 2 == 1)) {
        if (literal_at(m, p, sep_char(g))) {
          OKR(iset_add(m, &f->b, p + 1));
        }
      }
      else {
        ASK(kid_of(m, g, g->sep ? f->i / 2 : f->i), p, &e);
        OKR(iset_union_v(m, &f->b, e.v, e.n));
      }
      OKR(charge(m, 1));
      cursor_next(f);
    }
    iset_swap(&f->a, &f->b);
    f->b.n = 0;
    f->i++;
    f->state = S_LEVEL;
  }
}

static int step_mult(M * m, frame * f) {
  const gnode * g = &m->c->nodes[f->node];
  const uint32_t kid = kid_of(m, g, 0);
  ends_t e;
  if (f->state == S_BEGIN) {
    if (!f->has_start) {
      OKR(iset_add(m, &f->a, f->pos));
    }
    if (g->min == 0) {
      OKR(iset_union_v(m, &f->r, f->a.v, f->a.n));
    }
    f->i = 0;
    f->state = S_LEVEL;
  }
  for (;;) {
    if (f->state == S_LEVEL) {
      const uint32_t kk = f->i + 1;
      if ((g->max != GN_INF && kk > g->max) || f->a.n == 0) {
        return finish(m, f);
      }
      f->b.n = 0;
      cursor_start(f);
      f->state = S_EXPAND;
    }
    while (f->cvalid) {
      const uint32_t kk = f->i + 1;
      uint32_t q = f->cp;
      if (is_closure(g)) {
        /* a position this multiplier has already been worked from: everything reachable from it is known */
        const mentry * done = memo_find(m, f->node, q);
        if (done) {
          OKR(iset_union_v(m, &f->r, (const ival *)css_ptr_at(m->pool, done->off, sizeof(ival)), done->len));
          OKR(iset_union_v(m, &f->s, (const ival *)css_ptr_at(m->pool, done->off, sizeof(ival)), done->len));
          OKR(charge(m, 1));
          cursor_next(f);
          continue;
        }
      }
      if ((g->flags & GNF_COMMA) && kk >= 2) {
        if (!literal_at(m, q, ',')) {
          cursor_next(f);
          continue;
        }
        q++;
      }
      ASK(kid, q, &e);
      OKR(iset_union_v(m, &f->b, e.v, e.n));
      OKR(charge(m, 1));
      cursor_next(f);
    }
    const uint32_t kk = f->i + 1;
    if (f->b.n == 0) {
      return finish(m, f);
    }
    if (kk >= g->min) {
      OKR(iset_union_v(m, &f->r, f->b.v, f->b.n));
    }
    if (g->max == GN_INF && kk >= (g->min > 1 ? g->min : 1)) {
      OKR(iset_subtract(m, &f->b, &f->s));
      OKR(iset_union_v(m, &f->s, f->b.v, f->b.n));
      if (f->b.n == 0) {
        return finish(m, f);
      }
    }
    else if (kk > g->min && iset_equal(&f->a, &f->b)) {
      return finish(m, f);
    }
    iset_swap(&f->a, &f->b);
    f->b.n = 0;
    f->i = kk;
    f->state = S_LEVEL;
  }
}

static size_t state_hash(uint32_t pos, uint64_t mask) {
  uint64_t k = mask * 0x9E3779B97F4A7C15ULL ^ ((uint64_t)pos << 17) ^ pos;
  k ^= k >> 31;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 29;
  return (size_t)k;
}

/// Add state (pos, mask) to an && / || frame if new.
static bool add_state(M * m, frame * f, const gnode * g, uint32_t pos, uint64_t mask, uint64_t full) {
  if ((f->nst + 1) * 2 > f->nsh) {
    const size_t nsh = f->nsh ? f->nsh * 2 : 16;
    uint32_t * sh = gtext_allocator_calloc(&m->alloc, nsh, sizeof *sh);
    if (!sh) {
      fail_oom(m);
      return false;
    }
    for (size_t k = 0; k < f->nst; k++) {
      size_t i = state_hash(f->st[k].pos, f->st[k].mask) & (nsh - 1);
      while (sh[i]) {
        i = (i + 1) & (nsh - 1);
      }
      sh[i] = (uint32_t)(k + 1);
    }
    gtext_allocator_free(&m->alloc, f->sh);
    f->sh = sh;
    f->nsh = nsh;
  }
  size_t i = state_hash(pos, mask) & (f->nsh - 1);
  while (f->sh[i]) {
    if (f->st[f->sh[i] - 1].pos == pos && f->st[f->sh[i] - 1].mask == mask) {
      return true;
    }
    i = (i + 1) & (f->nsh - 1);
  }
  if (!memo_charge(m, 1) || !charge(m, 1)) {
    return false;
  }
  void * st = f->st;
  if (!css_grammar_grow(&m->alloc, &st, &f->stcap, f->nst + 1, sizeof f->st[0])) {
    fail_oom(m);
    return false;
  }
  f->st = st;
  f->st[f->nst].pos = pos;
  f->st[f->nst].mask = mask;
  f->nst++;
  f->sh[i] = (uint32_t)f->nst;
  if (g->kind == GN_AND ? mask == full : mask != 0) {
    return iset_add(m, &f->r, pos);
  }
  return true;
}

static uint64_t full_mask(uint32_t n) {
  return n >= 64 ? UINT64_MAX : (((uint64_t)1 << n) - 1);
}

static int step_andor(M * m, frame * f) {
  const gnode * g = &m->c->nodes[f->node];
  const uint64_t full = full_mask(g->nkids);
  ends_t e;
  if (f->state == S_BEGIN) {
    OKR(add_state(m, f, g, f->pos, 0, full));
    f->qi = 0;
    f->j = 0;
    f->state = S_LEVEL;
  }
  while (f->qi < f->nst) {
    const uint32_t spos = f->st[f->qi].pos;
    const uint64_t smask = f->st[f->qi].mask;
    while (f->j < g->nkids) {
      if (smask & ((uint64_t)1 << f->j)) {
        f->j++;
        continue;
      }
      ASK(kid_of(m, g, f->j), spos, &e);
      for (size_t k = 0; k < e.n; k++) {
        for (uint64_t p = e.v[k].lo; p <= e.v[k].hi; p++) {
          OKR(charge(m, 1)); /* (a position already a state costs a look too) */
          OKR(add_state(m, f, g, (uint32_t)p, smask | ((uint64_t)1 << f->j), full));
        }
      }
      f->j++;
    }
    f->qi++;
    f->j = 0;
  }
  return finish(m, f);
}

static int step(M * m, frame * f) {
  const gnode * g = &m->c->nodes[f->node];
  switch ((gn_kind)g->kind) {
  case GN_SEQ: return step_seq(m, f);
  case GN_ALT: return step_alt(m, f);
  case GN_AND: case GN_OR: return step_andor(m, f);
  case GN_MULT: return step_mult(m, f);
  case GN_REQ: return step_req(m, f);
  case GN_FUNC: case GN_BLOCK: return step_func(m, f);
  default:
    m->status = GTEXT_CSS_E_INTERNAL;
    return ST_ERR;
  }
}

/// Run until the answer for (node, pos) is in the memo. False (and the status) on failure.
static bool drive(M * m, uint32_t node, uint32_t pos) {
  const size_t base = m->nfr;
  if (!push_frame(m, node, pos)) {
    return false;
  }
  bool good = true;
  while (m->nfr > base && good) {
    frame * f = &m->fr[m->nfr - 1];
    const int r = step(m, f);
    if (r == ST_DONE) {
      pop_frame(m);
    }
    else if (r == ST_BLOCK) {
      frame * nf;
      if (m->need_set) {
        nf = push_frame(m, m->need_node, 0);
        if (nf) {
          nf->a = m->need_start;
          memset(&m->need_start, 0, sizeof m->need_start);
          nf->has_start = 1;
          nf->dest = m->need_dest;
        }
      }
      else {
        nf = push_frame(m, m->need_node, m->need_pos);
      }
      good = nf != NULL;
    }
    else {
      good = false;
    }
  }
  while (m->nfr > base) {
    pop_frame(m);
  }
  iset_free(m, &m->need_start);
  return good;
}

/// The ends of (node, pos), running the recognizer if they are not known yet.
static bool ends_sync(M * m, uint32_t node, uint32_t pos, ends_t * e) {
  int rc = get_ends(m, node, pos, e);
  if (rc == GET_BLOCK) {
    if (!drive(m, m->need_node, m->need_pos)) {
      return false;
    }
    rc = get_ends(m, node, pos, e);
  }
  return rc == GET_OK;
}

/// The positions reachable from every position of `start` through node `node` (a closure multiplier):
/// the recognizer's set-level run, for the extraction.
static bool set_sync(M * m, uint32_t node, const iset * start, iset * out) {
  const size_t base = m->nfr;
  frame * f = push_frame(m, node, 0);
  if (!f) {
    return false;
  }
  f->has_start = 1;
  f->dest = UINT32_MAX;
  if (!iset_union_v(m, &f->a, start->v, start->n)) {
    pop_frame(m);
    return false;
  }
  iset result = {NULL, 0, 0};
  m->sync_out = &result;
  bool good = true;
  while (m->nfr > base && good) {
    frame * top = &m->fr[m->nfr - 1];
    const int r = step(m, top);
    if (r == ST_DONE) {
      pop_frame(m);
    }
    else if (r == ST_BLOCK) {
      frame * nf;
      if (m->need_set) {
        nf = push_frame(m, m->need_node, 0);
        if (nf) {
          nf->a = m->need_start;
          memset(&m->need_start, 0, sizeof m->need_start);
          nf->has_start = 1;
          nf->dest = m->need_dest;
        }
      }
      else {
        nf = push_frame(m, m->need_node, m->need_pos);
      }
      good = nf != NULL;
    }
    else {
      good = false;
    }
  }
  while (m->nfr > base) {
    pop_frame(m);
  }
  m->sync_out = NULL;
  iset_free(m, &m->need_start);
  if (good) {
    iset_free(m, out);
    *out = result;
  }
  else {
    iset_free(m, &result);
  }
  return good;
}

/* ---- the typed value ------------------------------------------------------ */

typedef struct {
  M * m;
  GTEXT_CSS_Value_Set * set;
  GTEXT_CSS_Value * vals;
  size_t nvals, vcap;
  struct task { uint8_t type; uint32_t node, pos, end, base; } * tasks;
  size_t ntasks, tcap;
  bool unitless;
} builder;

enum { T_BUILD = 0, T_PACK };

static bool B_oom(builder * B) {
  fail_oom(B->m);
  return false;
}

static bool push_val(builder * B, const GTEXT_CSS_Value * v) {
  void * vals = B->vals;
  if (!css_grammar_grow(&B->m->alloc, &vals, &B->vcap, B->nvals + 1, sizeof *B->vals)) {
    return B_oom(B);
  }
  B->vals = vals;
  B->vals[B->nvals++] = *v;
  B->set->count++;
  return true;
}

static bool push_task(builder * B, uint8_t type, uint32_t node, uint32_t pos, uint32_t end, uint32_t base) {
  void * t = B->tasks;
  if (!css_grammar_grow(&B->m->alloc, &t, &B->tcap, B->ntasks + 1, sizeof *B->tasks)) {
    return B_oom(B);
  }
  B->tasks = t;
  B->tasks[B->ntasks].type = type;
  B->tasks[B->ntasks].node = node;
  B->tasks[B->ntasks].pos = pos;
  B->tasks[B->ntasks].end = end;
  B->tasks[B->ntasks].base = base;
  B->ntasks++;
  return charge(B->m, 1);
}

static const char * arena_text(builder * B, const char * s, size_t n, GTEXT_CSS_String * out) {
  char * c = css_arena_strdup(&B->set->arena, s, n);
  if (!c) {
    B_oom(B);
    return NULL;
  }
  out->data = c;
  out->length = n;
  return c;
}

/// Serialize component values into the set's arena.
static bool arena_serialize(builder * B, const GTEXT_CSS_Component_Value * values, size_t count, GTEXT_CSS_String * out) {
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &B->m->alloc;
  so.max_output_bytes = B->m->max_text;
  char * buf = NULL;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(values, count, &so, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    if (B->m->status == GTEXT_CSS_OK) {
      B->m->status = st; /* the real one: out of memory, or the limit on the text of a value */
    }
    return false;
  }
  const bool good = arena_text(B, buf, len, out) != NULL;
  gtext_css_serialize_buffer_free(&so, buf);
  return good;
}

static void value_init(GTEXT_CSS_Value * v, GTEXT_CSS_Val_Kind kind, uint32_t slot, const GTEXT_CSS_Component_Value * cv) {
  memset(v, 0, sizeof *v);
  v->kind = kind;
  v->slot = slot;
  v->interp = GTEXT_CSS_INTERP_DISCRETE;
  if (cv) {
    v->component = cv->id;
    v->span = cv->span;
  }
}

static GTEXT_CSS_Interp interp_of_unit(GTEXT_CSS_Unit u) {
  switch (gtext_css_unit_type(u)) {
  case GTEXT_CSS_UNIT_TYPE_LENGTH: return GTEXT_CSS_INTERP_LENGTH;
  case GTEXT_CSS_UNIT_TYPE_ANGLE: return GTEXT_CSS_INTERP_ANGLE;
  case GTEXT_CSS_UNIT_TYPE_TIME: return GTEXT_CSS_INTERP_TIME;
  default: return GTEXT_CSS_INTERP_DISCRETE;
  }
}

static GTEXT_CSS_Interp interp_of_native(gn_native t) {
  switch (t) {
  case NT_INTEGER: case NT_NUMBER: case NT_RATIO: case NT_ZERO: return GTEXT_CSS_INTERP_NUMBER;
  case NT_PERCENTAGE: return GTEXT_CSS_INTERP_PERCENTAGE;
  case NT_LENGTH: return GTEXT_CSS_INTERP_LENGTH;
  case NT_ANGLE: return GTEXT_CSS_INTERP_ANGLE;
  case NT_TIME: return GTEXT_CSS_INTERP_TIME;
  default: return GTEXT_CSS_INTERP_DISCRETE;
  }
}

static void number_flags(GTEXT_CSS_Value * v, const GTEXT_CSS_Component_Value * cv) {
  if (cv->flags & GTEXT_CSS_FLAG_NUMBER_INEXACT) {
    v->flags |= GTEXT_CSS_VFLAG_INEXACT;
  }
}

static bool span_cover(GTEXT_CSS_Value * v, const GTEXT_CSS_Component_Value * a, const GTEXT_CSS_Component_Value * b) {
  v->span = a->span;
  v->span.end = b->span.end;
  return true;
}

/// The name of a function component value in lower case (CSSOM writes a math function's name so), in the set's arena.
static bool lower_name(builder * B, const GTEXT_CSS_Component_Value * cv, GTEXT_CSS_String * out) {
  if (!arena_text(B, cv->value.data, cv->value.length, out)) {
    return false;
  }
  for (size_t i = 0; i < out->length; i++) {
    char * ch = (char *)out->data + i;
    if (*ch >= 'A' && *ch <= 'Z') {
      *ch = (char)(*ch + 32);
    }
  }
  return true;
}

/// The value of the terminal `node` that matched items `pos` up to (not including) `end`.
static bool terminal_value(builder * B, uint32_t node, uint32_t pos, uint32_t end, GTEXT_CSS_Value * out) {
  M * m = B->m;
  const gnode * g = &m->c->nodes[node];
  const mitem * it = &m->items[pos];
  const GTEXT_CSS_Component_Value * v = it->cv;
  if (g->kind == GN_KEYWORD) {
    value_init(out, GTEXT_CSS_VAL_KEYWORD, node, v);
    out->atom = g->atom;
    return arena_text(B, m->c->text + g->str_off, g->str_len, &out->text) != NULL;
  }
  if (g->kind == GN_LITERAL) {
    value_init(out, GTEXT_CSS_VAL_TOKEN, node, v);
    char b[4];
    const size_t n = gtext_css_encode_cp(g->lit, b);
    return arena_text(B, b, n, &out->text) != NULL;
  }
  const gn_native t = (gn_native)g->native;
  if (t == NT_TOK_WS) {
    value_init(out, GTEXT_CSS_VAL_TOKEN, node, NULL);
    return arena_text(B, " ", 1, &out->text) != NULL;
  }
  if (v && v->kind == GTEXT_CSS_VALUE_FUNCTION && css_grammar_native_numeric(t) && t != NT_DIMENSION) {
    /* a math function matched by type; its arguments are the math layer's */
    value_init(out, GTEXT_CSS_VAL_FUNCTION, node, v);
    out->flags = GTEXT_CSS_VFLAG_MATH;
    out->interp = interp_of_native(t);
    out->range_lo = (g->flags & GNF_RANGE) ? g->lo : -1.0 / 0.0;
    out->range_hi = (g->flags & GNF_RANGE) ? g->hi : 1.0 / 0.0;
    if (!lower_name(B, v, &out->text)) {
      return false;
    }
    GTEXT_CSS_Value * arg = css_arena_alloc(&B->set->arena, sizeof *arg);
    if (!arg) {
      return B_oom(B);
    }
    value_init(arg, GTEXT_CSS_VAL_UNPARSED, node, v);
    if (!arena_serialize(B, v->children, v->child_count, &arg->text)) {
      return false;
    }
    out->children = arg;
    out->child_count = 1;
    B->set->count++;
    return true;
  }
  switch (t) {
  case NT_INTEGER:
    value_init(out, GTEXT_CSS_VAL_INTEGER, node, v);
    out->number = v->number;
    out->interp = GTEXT_CSS_INTERP_NUMBER;
    number_flags(out, v);
    return true;
  case NT_NUMBER:
    value_init(out, GTEXT_CSS_VAL_NUMBER, node, v);
    out->number = v->number;
    out->interp = GTEXT_CSS_INTERP_NUMBER;
    if (v->flags & GTEXT_CSS_FLAG_INTEGER) {
      out->flags |= GTEXT_CSS_VFLAG_WRITTEN_INTEGER;
    }
    number_flags(out, v);
    return true;
  case NT_PERCENTAGE:
    value_init(out, GTEXT_CSS_VAL_PERCENTAGE, node, v);
    out->number = v->number;
    out->interp = GTEXT_CSS_INTERP_PERCENTAGE;
    number_flags(out, v);
    return true;
  case NT_DIMENSION: {
    value_init(out, GTEXT_CSS_VAL_DIMENSION, node, v);
    out->number = v->number;
    out->unit = unit_of(v);
    out->interp = interp_of_unit(out->unit);
    number_flags(out, v);
    return arena_text(B, v->unit.data, v->unit.length, &out->text) != NULL;
  }
  case NT_LENGTH: case NT_ANGLE: case NT_TIME: case NT_FREQUENCY: case NT_RESOLUTION: case NT_FLEX:
    value_init(out, GTEXT_CSS_VAL_DIMENSION, node, v);
    out->number = v->number;
    if (tok(v, GTEXT_CSS_TOKEN_NUMBER)) {
      out->unit = GTEXT_CSS_UNIT_PX;
      out->flags |= v->number == 0 ? GTEXT_CSS_VFLAG_ZERO_LENGTH : GTEXT_CSS_VFLAG_UNITLESS_LENGTH;
    }
    else {
      out->unit = unit_of(v);
    }
    out->interp = interp_of_native(t);
    number_flags(out, v);
    return true;
  case NT_RATIO: {
    value_init(out, GTEXT_CSS_VAL_RATIO, node, v);
    out->number = v->number;
    out->number2 = 1.0;
    out->interp = GTEXT_CSS_INTERP_NUMBER;
    const GTEXT_CSS_Component_Value * den = end == pos + 3 ? m->items[pos + 2].cv : NULL;
    if (den) {
      out->number2 = den->number;
      out->flags |= GTEXT_CSS_VFLAG_RATIO_DENOMINATOR;
      span_cover(out, v, den);
    }
    if (v->kind == GTEXT_CSS_VALUE_FUNCTION || (den && den->kind == GTEXT_CSS_VALUE_FUNCTION)) {
      /* a component that is a math function: the two components are the children, a number or the function */
      out->flags |= GTEXT_CSS_VFLAG_MATH;
      GTEXT_CSS_Value * kids = css_arena_alloc(&B->set->arena, 2 * sizeof *kids);
      if (!kids) {
        return B_oom(B);
      }
      for (int k = 0; k < 2; k++) {
        const GTEXT_CSS_Component_Value * comp = k == 0 ? v : den;
        if (!comp) { /* the denominator that was not written is 1 */
          value_init(&kids[k], GTEXT_CSS_VAL_NUMBER, node, NULL);
          kids[k].number = 1.0;
          kids[k].interp = GTEXT_CSS_INTERP_NUMBER;
        }
        else if (comp->kind == GTEXT_CSS_VALUE_FUNCTION) {
          value_init(&kids[k], GTEXT_CSS_VAL_FUNCTION, node, comp);
          kids[k].flags = GTEXT_CSS_VFLAG_MATH;
          kids[k].interp = GTEXT_CSS_INTERP_NUMBER;
          kids[k].range_lo = 0.0;
          kids[k].range_hi = 1.0 / 0.0;
          if (!lower_name(B, comp, &kids[k].text)) {
            return false;
          }
          GTEXT_CSS_Value * arg = css_arena_alloc(&B->set->arena, sizeof *arg);
          if (!arg) {
            return B_oom(B);
          }
          value_init(arg, GTEXT_CSS_VAL_UNPARSED, node, comp);
          if (!arena_serialize(B, comp->children, comp->child_count, &arg->text)) {
            return false;
          }
          kids[k].children = arg;
          kids[k].child_count = 1;
          B->set->count++;
        }
        else {
          value_init(&kids[k], GTEXT_CSS_VAL_NUMBER, node, comp);
          kids[k].number = comp->number;
          kids[k].interp = GTEXT_CSS_INTERP_NUMBER;
        }
      }
      out->children = kids;
      out->child_count = 2;
      B->set->count += 2;
    }
    return true;
  }
  case NT_ZERO:
    value_init(out, GTEXT_CSS_VAL_ZERO, node, v);
    out->number = 0;
    out->interp = GTEXT_CSS_INTERP_NUMBER;
    return true;
  case NT_STRING:
    value_init(out, GTEXT_CSS_VAL_STRING, node, v);
    return arena_text(B, v->value.data, v->value.length, &out->text) != NULL;
  case NT_URL: {
    value_init(out, GTEXT_CSS_VAL_URL, node, v);
    if (v->kind == GTEXT_CSS_VALUE_FUNCTION) {
      size_t seen = 0, mods = 0;
      for (size_t i = 0; i < v->child_count; i++) {
        const GTEXT_CSS_Component_Value * c = &v->children[i];
        if (is_ws(c)) {
          continue;
        }
        if (seen++ == 0) {
          if (!arena_text(B, c->value.data, c->value.length, &out->text)) {
            return false;
          }
        }
        else {
          mods++;
        }
      }
      if (mods) {
        GTEXT_CSS_Value * kids = css_arena_alloc(&B->set->arena, mods * sizeof *kids);
        if (!kids) {
          return B_oom(B);
        }
        size_t k = 0;
        seen = 0;
        for (size_t i = 0; i < v->child_count; i++) {
          const GTEXT_CSS_Component_Value * c = &v->children[i];
          if (is_ws(c)) {
            continue;
          }
          if (seen++ == 0) {
            continue;
          }
          value_init(&kids[k], GTEXT_CSS_VAL_TOKEN, node, c);
          if (!arena_serialize(B, c, 1, &kids[k].text)) {
            return false;
          }
          k++;
        }
        out->children = kids;
        out->child_count = mods;
        B->set->count += mods;
      }
      return true;
    }
    return arena_text(B, v->value.data, v->value.length, &out->text) != NULL;
  }
  case NT_IDENT:
    value_init(out, GTEXT_CSS_VAL_KEYWORD, node, v);
    out->atom = gtext_css_atom_lookup(m->c->atoms, v->value.data, v->value.length);
    return arena_text(B, v->value.data, v->value.length, &out->text) != NULL;
  case NT_CUSTOM_IDENT:
    value_init(out, GTEXT_CSS_VAL_CUSTOM_IDENT, node, v);
    return arena_text(B, v->value.data, v->value.length, &out->text) != NULL;
  case NT_DASHED_IDENT:
    value_init(out, GTEXT_CSS_VAL_DASHED_IDENT, node, v);
    return arena_text(B, v->value.data, v->value.length, &out->text) != NULL;
  case NT_URANGE:
    value_init(out, GTEXT_CSS_VAL_UNICODE_RANGE, node, v);
    out->range_start = v->range_start;
    out->range_end = v->range_end;
    return true;
  case NT_ANY_VALUE: case NT_DECL_VALUE: case NT_DECL_LIST: case NT_RULE_LIST: {
    value_init(out, GTEXT_CSS_VAL_UNPARSED, node, v);
    if (end > pos) {
      const mitem * last = &m->items[end - 1];
      span_cover(out, v, last->cv);
      return arena_serialize(B, v, (size_t)(last->orig - it->orig + 1), &out->text);
    }
    return arena_text(B, "", 0, &out->text) != NULL;
  }
  case NT_ANB: {
    value_init(out, GTEXT_CSS_VAL_UNPARSED, node, v);
    const mitem * last = &m->items[end - 1];
    int64_t a, b;
    if (!gtext_css_anb_parse(v, (size_t)(last->orig - it->orig + 1), &a, &b)) {
      m->status = GTEXT_CSS_E_INTERNAL;
      return false;
    }
    span_cover(out, v, last->cv);
    char txt[64];
    const size_t n = gtext_css_anb_serialize(a, b, txt, sizeof txt);
    return arena_text(B, txt, n < sizeof txt ? n : sizeof txt - 1, &out->text) != NULL;
  }
  default: /* the <*-token> types: the token, as written back */
    value_init(out, GTEXT_CSS_VAL_TOKEN, node, v);
    return arena_serialize(B, v, 1, &out->text);
  }
}

/* ---- finding the way a match went ----------------------------------------- */

typedef struct {
  uint32_t pos;     ///< The state's position.
  uint64_t key;     ///< A repetition count, or a mask of the members used.
  uint32_t parent;  ///< The state it came from (index), or UINT32_MAX.
  uint32_t j;       ///< && and ||: the member that was matched.
  uint32_t from;    ///< Where the step began.
} bnode;

typedef struct {
  bnode * n;
  size_t len, cap;
  uint32_t * h;
  size_t nh;
} bfs_t;

static void bfs_free(M * m, bfs_t * b) {
  gtext_allocator_free(&m->alloc, b->n);
  gtext_allocator_free(&m->alloc, b->h);
  memset(b, 0, sizeof *b);
}

/// Add a state; 1 + its index if it is new, 0 if it was there, -1 on failure.
static int64_t bfs_add(M * m, bfs_t * b, uint32_t pos, uint64_t key, uint32_t parent, uint32_t j, uint32_t from) {
  if ((b->len + 1) * 2 > b->nh) {
    const size_t nh = b->nh ? b->nh * 2 : 16;
    uint32_t * h = gtext_allocator_calloc(&m->alloc, nh, sizeof *h);
    if (!h) {
      fail_oom(m);
      return -1;
    }
    for (size_t k = 0; k < b->len; k++) {
      size_t i = state_hash(b->n[k].pos, b->n[k].key) & (nh - 1);
      while (h[i]) {
        i = (i + 1) & (nh - 1);
      }
      h[i] = (uint32_t)(k + 1);
    }
    gtext_allocator_free(&m->alloc, b->h);
    b->h = h;
    b->nh = nh;
  }
  size_t i = state_hash(pos, key) & (b->nh - 1);
  while (b->h[i]) {
    if (b->n[b->h[i] - 1].pos == pos && b->n[b->h[i] - 1].key == key) {
      return 0;
    }
    i = (i + 1) & (b->nh - 1);
  }
  if (!charge(m, 1)) {
    return -1;
  }
  void * n = b->n;
  if (!css_grammar_grow(&m->alloc, &n, &b->cap, b->len + 1, sizeof *b->n)) {
    fail_oom(m);
    return -1;
  }
  b->n = n;
  b->n[b->len] = (bnode){pos, key, parent, j, from};
  b->len++;
  b->h[i] = (uint32_t)b->len;
  return (int64_t)b->len;
}

/// A repetition's way: the (start, end) of each repetition, in order, into `*spans` (allocated, `*count` pairs).
static bool mult_path(builder * B, uint32_t node, uint32_t pos, uint32_t end, uint32_t ** spans, size_t * count) {
  M * m = B->m;
  const gnode * g = &m->c->nodes[node];
  const uint32_t kid = kid_of(m, g, 0);
  const bool comma = (g->flags & GNF_COMMA) != 0;
  *spans = NULL;
  *count = 0;
  if (pos == end && g->min == 0) {
    return true;
  }
  const uint32_t cap = g->max == GN_INF ? (g->min > 1 ? g->min : 1) : g->max;
  bfs_t b = {NULL, 0, 0, NULL, 0};
  int64_t found = 0;
  bool good = bfs_add(m, &b, pos, 0, UINT32_MAX, 0, pos) >= 0;
  for (size_t qi = 0; good && !found && qi < b.len; qi++) {
    const uint32_t kk = (uint32_t)b.n[qi].key;
    const uint32_t spos = b.n[qi].pos;
    if (!charge(m, 1)) {
      good = false;
      break;
    }
    if (g->max != GN_INF && kk + 1 > g->max) {
      continue;
    }
    uint32_t kk2 = kk + 1;
    if (g->max == GN_INF && kk2 > cap) {
      kk2 = cap;
    }
    uint32_t start = spos;
    if (comma && kk >= 1) {
      if (!literal_at(m, spos, ',')) {
        continue;
      }
      start = spos + 1;
    }
    ends_t e;
    if (!ends_sync(m, kid, start, &e)) {
      good = false;
      break;
    }
    for (size_t k = e.n; k-- > 0 && !found && good;) {
      for (uint64_t p = e.v[k].hi + 1; p-- > e.v[k].lo;) {
        if (!charge(m, 1)) {
          good = false;
          break;
        }
        const int64_t r = bfs_add(m, &b, (uint32_t)p, kk2, (uint32_t)qi, 0, start);
        if (r < 0) {
          good = false;
          break;
        }
        if (r > 0 && p == end && kk2 >= g->min) {
          found = r;
          break;
        }
      }
    }
  }
  if (good && !found) {
    m->status = GTEXT_CSS_E_INTERNAL;
    good = false;
  }
  if (good) {
    size_t reps = 0;
    for (int64_t i = found - 1; b.n[i].parent != UINT32_MAX; i = b.n[i].parent) {
      reps++;
    }
    uint32_t * out = gtext_allocator_malloc(&m->alloc, reps * 2 * sizeof *out);
    if (!out) {
      fail_oom(m);
      good = false;
    }
    else {
      size_t k = reps;
      for (int64_t i = found - 1; b.n[i].parent != UINT32_MAX; i = b.n[i].parent) {
        k--;
        out[2 * k] = b.n[i].from;
        out[2 * k + 1] = b.n[i].pos;
      }
      *spans = out;
      *count = reps;
    }
  }
  bfs_free(m, &b);
  return good;
}

/// The members of an && or || group that a match used: (member, start, end), by member.
static bool andor_path(builder * B, uint32_t node, uint32_t pos, uint32_t end, uint32_t ** members, size_t * count) {
  M * m = B->m;
  const gnode * g = &m->c->nodes[node];
  const uint64_t full = full_mask(g->nkids);
  *members = NULL;
  *count = 0;
  bfs_t b = {NULL, 0, 0, NULL, 0};
  int64_t found = 0;
  bool good = bfs_add(m, &b, pos, 0, UINT32_MAX, 0, pos) >= 0;
  for (size_t qi = 0; good && !found && qi < b.len; qi++) {
    const uint32_t spos = b.n[qi].pos;
    const uint64_t smask = b.n[qi].key;
    if (!charge(m, 1)) {
      good = false;
      break;
    }
    for (uint32_t j = 0; j < g->nkids && good && !found; j++) {
      if (smask & ((uint64_t)1 << j)) {
        continue;
      }
      ends_t e;
      if (!ends_sync(m, kid_of(m, g, j), spos, &e)) {
        good = false;
        break;
      }
      for (size_t k = e.n; k-- > 0 && !found && good;) {
        for (uint64_t p = e.v[k].hi + 1; p-- > e.v[k].lo;) {
          if (!charge(m, 1)) {
            good = false;
            break;
          }
          const uint64_t mask = smask | ((uint64_t)1 << j);
          const int64_t r = bfs_add(m, &b, (uint32_t)p, mask, (uint32_t)qi, j, spos);
          if (r < 0) {
            good = false;
            break;
          }
          if (r > 0 && p == end && (g->kind == GN_AND ? mask == full : mask != 0)) {
            found = r;
            break;
          }
        }
      }
    }
  }
  if (good && !found) {
    m->status = GTEXT_CSS_E_INTERNAL;
    good = false;
  }
  if (good) {
    size_t used = 0;
    for (int64_t i = found - 1; b.n[i].parent != UINT32_MAX; i = b.n[i].parent) {
      used++;
    }
    uint32_t * out = gtext_allocator_malloc(&m->alloc, (used ? used : 1) * 3 * sizeof *out);
    if (!out) {
      fail_oom(m);
      good = false;
    }
    else {
      /* by member: the members are distinct, so a selection by index is enough */
      for (int64_t i = found - 1; b.n[i].parent != UINT32_MAX; i = b.n[i].parent) {
        /* insert sorted (descending member, since the chain runs backwards, then reverse by position) */
        size_t at = 0;
        const size_t n_so_far = (size_t)(*count);
        while (at < n_so_far && out[3 * at] < b.n[i].j) {
          at++;
        }
        memmove(&out[3 * (at + 1)], &out[3 * at], (n_so_far - at) * 3 * sizeof *out);
        out[3 * at] = b.n[i].j;
        out[3 * at + 1] = b.n[i].from;
        out[3 * at + 2] = b.n[i].pos;
        (*count)++;
      }
      *members = out;
    }
  }
  bfs_free(m, &b);
  return good;
}

/// Whether step `i` of a SEQ can take position `p` to `to`.
static bool seq_step_has(builder * B, const gnode * g, uint32_t i, uint32_t p, uint32_t to) {
  M * m = B->m;
  if (g->sep && (i % 2 == 1)) {
    return literal_at(m, p, sep_char(g)) && p + 1 == to;
  }
  ends_t e;
  if (!ends_sync(m, kid_of(m, g, g->sep ? i / 2 : i), p, &e)) {
    return false;
  }
  return ends_has(&e, to);
}

/// The positions between the steps of a SEQ that matched (pos, end): `pts[0..nsteps]`.
static bool seq_path(builder * B, uint32_t node, uint32_t pos, uint32_t end, uint32_t * pts) {
  M * m = B->m;
  const gnode * g = &m->c->nodes[node];
  const uint32_t nsteps = seq_steps(g);
  iset * S = gtext_allocator_calloc(&m->alloc, nsteps + 1, sizeof *S);
  if (!S) {
    fail_oom(m);
    return false;
  }
  bool good = iset_add(m, &S[0], pos);
  for (uint32_t i = 0; good && i < nsteps; i++) {
    const bool is_sep = g->sep && (i % 2 == 1);
    const uint32_t kid = is_sep ? 0 : kid_of(m, g, g->sep ? i / 2 : i);
    if (!is_sep && is_closure(&m->c->nodes[kid]) && !iset_single(&S[i])) {
      good = set_sync(m, kid, &S[i], &S[i + 1]);
      continue;
    }
    for (size_t k = 0; good && k < S[i].n; k++) {
      for (uint64_t p = S[i].v[k].lo; good && p <= S[i].v[k].hi; p++) {
        if (is_sep) {
          good = charge(m, 1);
          if (good && literal_at(m, (uint32_t)p, sep_char(g))) {
            good = iset_add(m, &S[i + 1], (uint32_t)p + 1);
          }
        }
        else {
          ends_t e;
          good = ends_sync(m, kid, (uint32_t)p, &e) && iset_union_v(m, &S[i + 1], e.v, e.n) && charge(m, 1);
        }
      }
    }
  }
  if (good && !iset_has(&S[nsteps], end)) {
    m->status = GTEXT_CSS_E_INTERNAL;
    good = false;
  }
  if (good) {
    uint32_t cur = end;
    pts[nsteps] = end;
    for (uint32_t i = nsteps; good && i-- > 0;) {
      bool chosen = false;
      for (size_t k = S[i].n; k-- > 0 && !chosen && good;) {
        uint64_t p = S[i].v[k].hi;
        if (p > cur) {
          if (S[i].v[k].lo > cur) {
            continue;
          }
          p = cur;
        }
        for (p = p + 1; p-- > S[i].v[k].lo;) {
          if (!charge(m, 1)) {
            good = false;
            break;
          }
          if (seq_step_has(B, g, i, (uint32_t)p, cur)) {
            cur = (uint32_t)p;
            pts[i] = cur;
            chosen = true;
            break;
          }
          if (m->status != GTEXT_CSS_OK) {
            good = false;
            break;
          }
        }
      }
      if (good && !chosen) {
        m->status = GTEXT_CSS_E_INTERNAL;
        good = false;
      }
    }
  }
  for (uint32_t i = 0; i <= nsteps; i++) {
    iset_free(m, &S[i]);
  }
  gtext_allocator_free(&m->alloc, S);
  return good;
}

/* ---- packing values ----------------------------------------------------- */

static bool pack_list(builder * B, uint32_t node, size_t base, GTEXT_CSS_List_Separator sep) {
  const size_t n = B->nvals - base;
  if (n == 0) {
    return true;
  }
  GTEXT_CSS_Value * kids = css_arena_alloc(&B->set->arena, n * sizeof *kids);
  if (!kids) {
    return B_oom(B);
  }
  memcpy(kids, &B->vals[base], n * sizeof *kids);
  B->nvals = base;
  GTEXT_CSS_Value list;
  value_init(&list, GTEXT_CSS_VAL_LIST, node, NULL);
  list.separator = sep;
  list.interp = GTEXT_CSS_INTERP_LIST;
  list.children = kids;
  list.child_count = n;
  /* from the first child that has a span to the last that has one (a <whitespace-token>, or an empty run, has none) */
  size_t first = 0, last = n;
  while (first < n && kids[first].span.line == 0) {
    first++;
  }
  while (last > first && kids[last - 1].span.line == 0) {
    last--;
  }
  if (first < last) {
    list.span = kids[first].span;
    if (kids[last - 1].span.end > list.span.end) {
      list.span.end = kids[last - 1].span.end;
    }
  }
  return push_val(B, &list);
}

static bool pack_func(builder * B, uint32_t node, uint32_t pos, size_t base) {
  M * m = B->m;
  const gnode * g = &m->c->nodes[node];
  const GTEXT_CSS_Component_Value * cv = m->items[pos].cv;
  const size_t n = B->nvals - base;
  GTEXT_CSS_Value f;
  value_init(&f, g->kind == GN_FUNC ? GTEXT_CSS_VAL_FUNCTION : GTEXT_CSS_VAL_BLOCK, node, cv);
  if (g->kind == GN_FUNC) {
    if (!arena_text(B, m->c->text + g->str_off, g->str_len, &f.text)) {
      return false;
    }
  }
  else {
    const char * open = g->native == GTEXT_CSS_TOKEN_OPEN_CURLY ? "{" : g->native == GTEXT_CSS_TOKEN_OPEN_SQUARE ? "[" : "(";
    if (!arena_text(B, open, 1, &f.text)) {
      return false;
    }
  }
  if (n) {
    GTEXT_CSS_Value * kids = css_arena_alloc(&B->set->arena, n * sizeof *kids);
    if (!kids) {
      return B_oom(B);
    }
    memcpy(kids, &B->vals[base], n * sizeof *kids);
    f.children = kids;
    f.child_count = n;
  }
  B->nvals = base;
  return push_val(B, &f);
}

/// Do one task of the extraction.
static bool build_task(builder * B, const struct task * t) {
  M * m = B->m;
  const gnode * g = &m->c->nodes[t->node];
  if (!charge(m, 1)) {
    return false;
  }
  if (t->type == T_PACK) {
    switch ((gn_kind)g->kind) {
    case GN_SEQ:
      return pack_list(B, t->node, t->base, g->sep == 1 ? GTEXT_CSS_SEP_COMMA : g->sep == 2 ? GTEXT_CSS_SEP_SLASH : GTEXT_CSS_SEP_SPACE);
    case GN_MULT:
      if (g->max == 1) {
        return true;
      }
      return pack_list(B, t->node, t->base, (g->flags & GNF_COMMA) ? GTEXT_CSS_SEP_COMMA : GTEXT_CSS_SEP_SPACE);
    case GN_AND: case GN_OR:
      return pack_list(B, t->node, t->base, GTEXT_CSS_SEP_SPACE);
    case GN_FUNC: case GN_BLOCK:
      return pack_func(B, t->node, t->pos, t->base);
    default:
      return true;
    }
  }
  switch ((gn_kind)g->kind) {
  case GN_KEYWORD: case GN_LITERAL: case GN_NATIVE: {
    GTEXT_CSS_Value v;
    return terminal_value(B, t->node, t->pos, t->end, &v) && push_val(B, &v);
  }
  case GN_REQ:
    return push_task(B, T_BUILD, kid_of(m, g, 0), t->pos, t->end, 0);
  case GN_ALT: {
    for (uint32_t j = 0; j < g->nkids; j++) {
      ends_t e;
      if (!ends_sync(m, kid_of(m, g, j), t->pos, &e)) {
        return false;
      }
      if (ends_has(&e, t->end)) {
        return push_task(B, T_BUILD, kid_of(m, g, j), t->pos, t->end, 0);
      }
    }
    m->status = GTEXT_CSS_E_INTERNAL;
    return false;
  }
  case GN_SEQ: {
    const uint32_t nsteps = seq_steps(g);
    uint32_t * pts = gtext_allocator_malloc(&m->alloc, ((size_t)nsteps + 1) * sizeof *pts);
    if (!pts) {
      return B_oom(B);
    }
    bool good = seq_path(B, t->node, t->pos, t->end, pts)
        && push_task(B, T_PACK, t->node, t->pos, t->end, (uint32_t)B->nvals);
    for (uint32_t i = nsteps; good && i-- > 0;) {
      if (g->sep && (i % 2 == 1)) {
        continue;
      }
      good = push_task(B, T_BUILD, kid_of(m, g, g->sep ? i / 2 : i), pts[i], pts[i + 1], 0);
    }
    gtext_allocator_free(&m->alloc, pts);
    return good;
  }
  case GN_MULT: {
    uint32_t * spans;
    size_t count;
    if (!mult_path(B, t->node, t->pos, t->end, &spans, &count)) {
      return false;
    }
    bool good = push_task(B, T_PACK, t->node, t->pos, t->end, (uint32_t)B->nvals);
    for (size_t i = count; good && i-- > 0;) {
      good = push_task(B, T_BUILD, kid_of(m, g, 0), spans[2 * i], spans[2 * i + 1], 0);
    }
    gtext_allocator_free(&m->alloc, spans);
    return good;
  }
  case GN_AND: case GN_OR: {
    uint32_t * mem;
    size_t count;
    if (!andor_path(B, t->node, t->pos, t->end, &mem, &count)) {
      return false;
    }
    bool good = push_task(B, T_PACK, t->node, t->pos, t->end, (uint32_t)B->nvals);
    for (size_t i = count; good && i-- > 0;) {
      good = push_task(B, T_BUILD, kid_of(m, g, mem[3 * i]), mem[3 * i + 1], mem[3 * i + 2], 0);
    }
    gtext_allocator_free(&m->alloc, mem);
    return good;
  }
  case GN_FUNC: case GN_BLOCK: {
    bool good = push_task(B, T_PACK, t->node, t->pos, t->end, (uint32_t)B->nvals);
    if (good && g->nkids) {
      const uint32_t cs = m->items[t->pos].child;
      good = push_task(B, T_BUILD, kid_of(m, g, 0), cs, m->items[cs].end, 0);
    }
    return good;
  }
  default:
    m->status = GTEXT_CSS_E_INTERNAL;
    return false;
  }
}

/* ---- the entry points ------------------------------------------------------ */

static void m_free(M * m) {
  gtext_allocator_free(&m->alloc, m->items);
  gtext_allocator_free(&m->alloc, m->htab);
  gtext_allocator_free(&m->alloc, m->ents);
  gtext_allocator_free(&m->alloc, m->pool);
  for (size_t i = 0; i < m->nfr; i++) {
    frame_free(m, &m->fr[i]);
  }
  gtext_allocator_free(&m->alloc, m->fr);
  iset_free(m, &m->need_start);
}

static void match_result(GTEXT_CSS_Match_Result * result, const M * m, GTEXT_CSS_Status status) {
  if (!result) {
    return;
  }
  memset(result, 0, sizeof *result);
  result->status = status;
  result->message = gtext_css_status_string(status);
  result->work = m ? m->work : 0;
  result->memo_entries = m ? m->memo_peak : 0;
  result->positions = m && m->nitems ? m->nitems : 0;
}

static GTEXT_CSS_Value_Set * new_set(const GTEXT_Allocator * alloc) {
  GTEXT_CSS_Value_Set * set = gtext_allocator_malloc(alloc, sizeof *set);
  if (!set) {
    return NULL;
  }
  memset(set, 0, sizeof *set);
  set->alloc = *alloc;
  css_arena_init(&set->arena, &set->alloc);
  return set;
}

/// The value of a pending-substitution match.
static GTEXT_CSS_Status pending_set(M * m, const GTEXT_CSS_Component_Value * values, size_t count, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * set = new_set(&m->alloc);
  if (!set) {
    return GTEXT_CSS_E_OOM;
  }
  builder B;
  memset(&B, 0, sizeof B);
  B.m = m;
  B.set = set;
  GTEXT_CSS_Value * v = css_arena_alloc(&set->arena, sizeof *v);
  if (!v) {
    gtext_css_value_set_free(set);
    return GTEXT_CSS_E_OOM;
  }
  /* the first and last non-white-space values bound the span */
  size_t first = 0, last = count;
  while (first < count && is_ws(&values[first])) {
    first++;
  }
  while (last > first && is_ws(&values[last - 1])) {
    last--;
  }
  value_init(v, GTEXT_CSS_VAL_PENDING, m->c->root, first < count ? &values[first] : NULL);
  if (last > first) {
    span_cover(v, &values[first], &values[last - 1]);
  }
  if (!arena_serialize(&B, values, count, &v->text)) {
    const GTEXT_CSS_Status st = m->status == GTEXT_CSS_OK ? GTEXT_CSS_E_INTERNAL : m->status;
    gtext_css_value_set_free(set);
    return st;
  }
  set->root = v;
  set->count = 1;
  *out = set;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_match(const GTEXT_CSS_Compiled * compiled, const GTEXT_CSS_Component_Value * values,
    size_t count, const GTEXT_CSS_Match_Options * options, bool * matched, GTEXT_CSS_Value_Set ** out,
    GTEXT_CSS_Match_Result * result) {
  if (!compiled || !matched || (!values && count)) {
    match_result(result, NULL, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  M m;
  memset(&m, 0, sizeof m);
  m.c = compiled;
  m.alloc = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
  m.max_work = (options && options->max_match_work) ? options->max_match_work : DEFAULT_MAX_MATCH_WORK;
  m.max_memo = (options && options->max_memo_entries) ? options->max_memo_entries : DEFAULT_MAX_MEMO_ENTRIES;
  if (m.max_memo > GN_MAX_MEMO) {
    m.max_memo = GN_MAX_MEMO; /* an entry's place in the pool is 32 bits */
  }
  m.max_text = (options && options->max_text_bytes) ? options->max_text_bytes : ((size_t)256 << 20);
  m.unitless = options && options->unitless_length;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  GTEXT_CSS_Value_Set * set = NULL;
  bool did_match = false;
  bool pending = false;
  if (count) {
    status = gtext_css_has_arbitrary_substitution(values, count, &m.alloc, &pending);
  }
  if (status == GTEXT_CSS_OK && pending) {
    did_match = true;
    if (out) {
      status = pending_set(&m, values, count, &set);
    }
  }
  else if (status == GTEXT_CSS_OK) {
    if (!build_items(&m, values, count)) {
      status = m.status == GTEXT_CSS_OK ? GTEXT_CSS_E_INTERNAL : m.status;
    }
    else {
      ends_t e;
      if (!ends_sync(&m, compiled->root, 0, &e)) {
        status = m.status == GTEXT_CSS_OK ? GTEXT_CSS_E_INTERNAL : m.status;
      }
      else if (ends_has(&e, m.items[0].end)) {
        did_match = true;
        if (out) {
          builder B;
          memset(&B, 0, sizeof B);
          B.m = &m;
          set = new_set(&m.alloc);
          if (!set) {
            status = GTEXT_CSS_E_OOM;
          }
          else {
            B.set = set;
            bool good = push_task(&B, T_BUILD, compiled->root, 0, m.items[0].end, 0);
            while (good && B.ntasks > 0) {
              struct task t = B.tasks[--B.ntasks];
              good = build_task(&B, &t);
            }
            if (good) {
              if (B.nvals > 1) {
                m.status = GTEXT_CSS_E_INTERNAL;
                good = false;
              }
              else if (B.nvals == 1) {
                GTEXT_CSS_Value * root = css_arena_alloc(&set->arena, sizeof *root);
                if (!root) {
                  fail_oom(&m);
                  good = false;
                }
                else {
                  *root = B.vals[0];
                  set->root = root;
                }
              }
            }
            gtext_allocator_free(&m.alloc, B.vals);
            gtext_allocator_free(&m.alloc, B.tasks);
            if (!good) {
              status = m.status == GTEXT_CSS_OK ? GTEXT_CSS_E_INTERNAL : m.status;
              gtext_css_value_set_free(set);
              set = NULL;
            }
          }
        }
      }
    }
  }
  match_result(result, &m, status);
  if (result) {
    result->positions = m.nitems;
  }
  m_free(&m);
  if (status != GTEXT_CSS_OK) {
    gtext_css_value_set_free(set);
    return status;
  }
  *matched = did_match;
  if (out) {
    *out = set;
  }
  return GTEXT_CSS_OK;
}

/* ---- the hook ---------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_matcher_init(GTEXT_CSS_Matcher * matcher, const GTEXT_CSS_Compiled * compiled,
    const GTEXT_CSS_Match_Options * options) {
  if (!matcher || !compiled) {
    return GTEXT_CSS_E_INVALID;
  }
  memset(matcher, 0, sizeof *matcher);
  matcher->compiled = compiled;
  matcher->options = options ? *options : gtext_css_match_options_default();
  /* the allocator is copied: a NULL means the default, and the caller's struct need not outlive the matcher */
  matcher->allocator = *(matcher->options.allocator ? matcher->options.allocator : gtext_allocator_default());
  matcher->options.allocator = NULL;
  matcher->status = GTEXT_CSS_OK;
  return GTEXT_CSS_OK;
}

void gtext_css_matcher_free(GTEXT_CSS_Matcher * matcher) {
  if (!matcher) {
    return;
  }
  for (size_t i = 0; i < matcher->set_count; i++) {
    gtext_css_value_set_free(matcher->sets[i]);
  }
  gtext_allocator_free(&matcher->allocator, matcher->sets);
  matcher->sets = NULL;
  matcher->set_count = 0;
}

static bool matcher_match(void * user, size_t index, const GTEXT_CSS_Component_Value * values, size_t count) {
  GTEXT_CSS_Matcher * mt = user;
  if (index == 0) {
    gtext_css_matcher_free(mt);
  }
  bool matched = false;
  GTEXT_CSS_Value_Set * set = NULL;
  GTEXT_CSS_Match_Options o = mt->options;
  o.allocator = &mt->allocator;
  mt->status = gtext_css_match(mt->compiled, values, count, &o, &matched, &set, NULL);
  if (mt->status != GTEXT_CSS_OK || !matched) {
    gtext_css_value_set_free(set);
    return false;
  }
  if (index >= mt->set_count) {
    void * sets = mt->sets;
    size_t cap = mt->set_count;
    if (!css_grammar_grow(&mt->allocator, &sets, &cap, index + 1, sizeof mt->sets[0])) {
      gtext_css_value_set_free(set);
      mt->status = GTEXT_CSS_E_OOM;
      return false;
    }
    mt->sets = sets;
    for (size_t i = mt->set_count; i < cap; i++) {
      mt->sets[i] = NULL;
    }
    mt->set_count = cap;
  }
  gtext_css_value_set_free(mt->sets[index]);
  mt->sets[index] = set;
  return true;
}

GTEXT_CSS_Grammar gtext_css_matcher_grammar(GTEXT_CSS_Matcher * matcher) {
  GTEXT_CSS_Grammar g;
  g.match = matcher_match;
  g.user = matcher;
  return g;
}

const GTEXT_CSS_Value_Set * gtext_css_matcher_value_set(const GTEXT_CSS_Matcher * matcher, size_t index) {
  if (!matcher || index >= matcher->set_count) {
    return NULL;
  }
  return matcher->sets[index];
}
