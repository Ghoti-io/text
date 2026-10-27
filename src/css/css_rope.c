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
 * Token ropes (css_rope.h).
 */

#include "css_rope.h"

#include <stdlib.h>
#include <string.h>

enum { KIND_LEAF = 0, KIND_CAT = 1 };
enum {
  FLAG_TAINT_ALL = 0x1, ///< Every byte under this node is tainted.
  FLAG_HAS_TAINT = 0x2  ///< Some byte under it is.
};

struct css_rope {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  css_rope * a;       ///< A concatenation's first half (owned), else NULL.
  css_rope * b;       ///< Its second half (owned).
  css_rope * link;    ///< The worklist of a release in progress.
  uint64_t tokens;
  uint64_t bytes;
  size_t length;      ///< A leaf's bytes in `data`.
  uint8_t kind;
  uint8_t flags;
  uint8_t first;
  uint8_t last;
  char data[];
};

uint64_t css_sat_add(uint64_t a, uint64_t b) {
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

bool css_tokclass_needs_comment(css_tokclass last, css_tokclass first) {
  if (last == CSS_TC_NONE || last == CSS_TC_SAFE || first == CSS_TC_NONE || first == CSS_TC_SAFE) {
    return false;
  }
  switch (last) {
  case CSS_TC_IDENT:
    /* "ident followed by ident, function, url, bad-url, '-', number, percentage, dimension, CDC or '('" */
    return first == CSS_TC_IDENT || first == CSS_TC_FUNC || first == CSS_TC_URL || first == CSS_TC_BADURL || first == CSS_TC_D_MINUS ||
        first == CSS_TC_NUM || first == CSS_TC_PCT || first == CSS_TC_DIM || first == CSS_TC_CDC || first == CSS_TC_LPAREN;
  case CSS_TC_ATKW:
  case CSS_TC_HASH:
  case CSS_TC_DIM:
    /* "at-keyword, hash, dimension followed by ident, function, url, bad-url, '-', number, percentage, dimension or CDC" */
    return first == CSS_TC_IDENT || first == CSS_TC_FUNC || first == CSS_TC_URL || first == CSS_TC_BADURL || first == CSS_TC_D_MINUS ||
        first == CSS_TC_NUM || first == CSS_TC_PCT || first == CSS_TC_DIM || first == CSS_TC_CDC;
  case CSS_TC_D_HASH:
  case CSS_TC_D_MINUS:
    /* "'#' and '-' followed by ident, function, url, bad-url, '-', number, percentage or dimension" */
    return first == CSS_TC_IDENT || first == CSS_TC_FUNC || first == CSS_TC_URL || first == CSS_TC_BADURL || first == CSS_TC_D_MINUS ||
        first == CSS_TC_NUM || first == CSS_TC_PCT || first == CSS_TC_DIM;
  case CSS_TC_NUM:
    /* "number followed by ident, function, url, bad-url, number, percentage, dimension or '%'" */
    return first == CSS_TC_IDENT || first == CSS_TC_FUNC || first == CSS_TC_URL || first == CSS_TC_BADURL || first == CSS_TC_NUM ||
        first == CSS_TC_PCT || first == CSS_TC_DIM || first == CSS_TC_D_PCT;
  case CSS_TC_D_AT:
    /* "'@' followed by ident, function, url, bad-url or '-'" */
    return first == CSS_TC_IDENT || first == CSS_TC_FUNC || first == CSS_TC_URL || first == CSS_TC_BADURL || first == CSS_TC_D_MINUS;
  case CSS_TC_D_DOT:
  case CSS_TC_D_PLUS:
    /* "'.' and '+' followed by number, percentage or dimension" */
    return first == CSS_TC_NUM || first == CSS_TC_PCT || first == CSS_TC_DIM;
  case CSS_TC_D_SLASH:
    /* "'/' followed by '*'" */
    return first == CSS_TC_D_STAR;
  default:
    return false;
  }
}

static css_rope * node_new(const GTEXT_Allocator * alloc, size_t length) {
  if (length > SIZE_MAX - sizeof(css_rope) - 1) {
    return NULL;
  }
  css_rope * n = gtext_allocator_calloc((GTEXT_Allocator *)alloc, 1, sizeof *n + length + 1);
  if (!n) {
    return NULL;
  }
  gcu_atomic_int_init(&n->refs, 1);
  n->alloc = *alloc;
  return n;
}

css_rope * css_rope_leaf(const GTEXT_Allocator * alloc, const char * bytes, size_t length, css_tokclass first, css_tokclass last, uint64_t tokens,
    bool tainted) {
  css_rope * n = node_new(alloc, length);
  if (!n) {
    return NULL;
  }
  n->kind = KIND_LEAF;
  if (length) {
    memcpy(n->data, bytes, length);
  }
  n->length = length;
  n->bytes = length;
  n->tokens = tokens;
  n->first = length ? (uint8_t)first : (uint8_t)CSS_TC_NONE;
  n->last = length ? (uint8_t)last : (uint8_t)CSS_TC_NONE;
  if (tainted && length) {
    n->flags = FLAG_TAINT_ALL | FLAG_HAS_TAINT;
  }
  return n;
}

css_rope * css_rope_retain(css_rope * r) {
  if (r) {
    (void)gcu_atomic_int_fetch_add(&r->refs, 1);
  }
  return r;
}

static css_rope * make_cat(const GTEXT_Allocator * alloc, css_rope * a, css_rope * b) {
  css_rope * n = node_new(alloc, 0);
  if (!n) {
    return NULL;
  }
  n->kind = KIND_CAT;
  n->a = css_rope_retain(a);
  n->b = css_rope_retain(b);
  n->bytes = css_sat_add(a->bytes, b->bytes);
  n->tokens = css_sat_add(a->tokens, b->tokens);
  n->first = a->first;
  n->last = b->last;
  n->flags = (uint8_t)(((a->flags | b->flags) & FLAG_HAS_TAINT) | ((a->flags & b->flags & FLAG_TAINT_ALL) ? FLAG_TAINT_ALL : 0));
  return n;
}

css_rope * css_rope_cat(const GTEXT_Allocator * alloc, css_rope * a, css_rope * b, css_rope * comment) {
  if (!a || a->bytes == 0) {
    return css_rope_retain(b);
  }
  if (!b || b->bytes == 0) {
    return css_rope_retain(a);
  }
  if (!css_tokclass_needs_comment((css_tokclass)a->last, (css_tokclass)b->first)) {
    return make_cat(alloc, a, b);
  }
  css_rope * own = NULL;
  if (!comment) {
    own = css_rope_leaf(alloc, "/**/", 4, CSS_TC_SAFE, CSS_TC_SAFE, 0, false);
    if (!own) {
      return NULL;
    }
    comment = own;
  }
  css_rope * left = make_cat(alloc, a, comment);
  css_rope_release(own);
  if (!left) {
    return NULL;
  }
  css_rope * whole = make_cat(alloc, left, b);
  css_rope_release(left);
  return whole;
}

css_rope * css_rope_taint(const GTEXT_Allocator * alloc, css_rope * r) {
  if (!r || r->bytes == 0 || (r->flags & FLAG_TAINT_ALL)) {
    return css_rope_retain(r);
  }
  /* a node that says "everything under me is tainted": a concatenation of the rope with nothing, marked */
  css_rope * n = node_new(alloc, 0);
  if (!n) {
    return NULL;
  }
  n->kind = KIND_CAT;
  n->a = css_rope_retain(r);
  n->b = NULL;
  n->bytes = r->bytes;
  n->tokens = r->tokens;
  n->first = r->first;
  n->last = r->last;
  n->flags = FLAG_TAINT_ALL | FLAG_HAS_TAINT;
  return n;
}

void css_rope_release(css_rope * r) {
  if (!r) {
    return;
  }
  css_rope * work = NULL;
  if (gcu_atomic_int_fetch_sub(&r->refs, 1) != 1) {
    return;
  }
  r->link = NULL;
  work = r;
  while (work) {
    css_rope * n = work;
    work = n->link;
    css_rope * kids[2] = {n->a, n->b};
    GTEXT_Allocator a = n->alloc;
    gtext_allocator_free(&a, n);
    for (int k = 0; k < 2; k++) {
      css_rope * c = kids[k];
      if (c && gcu_atomic_int_fetch_sub(&c->refs, 1) == 1) {
        c->link = work;
        work = c;
      }
    }
  }
}

uint64_t css_rope_tokens(const css_rope * r) {
  return r ? r->tokens : 0;
}

uint64_t css_rope_bytes(const css_rope * r) {
  return r ? r->bytes : 0;
}

bool css_rope_has_taint(const css_rope * r) {
  return r && (r->flags & FLAG_HAS_TAINT) != 0;
}

css_tokclass css_rope_first(const css_rope * r) {
  return r ? (css_tokclass)r->first : CSS_TC_NONE;
}

css_tokclass css_rope_last(const css_rope * r) {
  return r ? (css_tokclass)r->last : CSS_TC_NONE;
}

/* ---- writing ------------------------------------------------------------------------------------- */

typedef struct {
  const css_rope ** items;
  size_t len, cap;
} nstack;

static bool nstack_push(const GTEXT_Allocator * alloc, nstack * s, const css_rope * n) {
  if (s->len == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 64;
    if (cap > SIZE_MAX / sizeof *s->items) {
      return false;
    }
    const css_rope ** g = gtext_allocator_realloc((GTEXT_Allocator *)alloc, s->items, cap * sizeof *g);
    if (!g) {
      return false;
    }
    s->items = g;
    s->cap = cap;
  }
  s->items[s->len++] = n;
  return true;
}

typedef struct {
  const GTEXT_Allocator * alloc;
  css_rope_sink sink;
  void * user;
  char * buf;       ///< Flatten: the output.
  size_t pos;
  size_t cap;
  size_t max;
  css_range * taint;
  size_t ntaint, taintcap;
  GTEXT_CSS_Status status;
} writer;

static bool taint_add(writer * w, size_t begin, size_t end) {
  if (begin == end) {
    return true;
  }
  if (w->ntaint && w->taint[w->ntaint - 1].end >= begin) {
    if (end > w->taint[w->ntaint - 1].end) {
      w->taint[w->ntaint - 1].end = end;
    }
    return true;
  }
  if (w->ntaint == w->taintcap) {
    const size_t cap = w->taintcap ? w->taintcap * 2 : 8;
    css_range * g = gtext_allocator_realloc((GTEXT_Allocator *)w->alloc, w->taint, cap * sizeof *g);
    if (!g) {
      w->status = GTEXT_CSS_E_OOM;
      return false;
    }
    w->taint = g;
    w->taintcap = cap;
  }
  w->taint[w->ntaint].begin = begin;
  w->taint[w->ntaint].end = end;
  w->ntaint++;
  return true;
}

/// Walk the rope, calling `emit` for each leaf in order. A node marked tainted records its range once at the position it starts.
static GTEXT_CSS_Status walk(writer * w, const css_rope * root, size_t * work, bool (*emit)(writer *, const char *, size_t), bool track_taint) {
  nstack st = {0};
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  if (!nstack_push(w->alloc, &st, root)) {
    return GTEXT_CSS_E_OOM;
  }
  size_t at = 0; /* the bytes emitted so far, for the taint ranges */
  while (st.len) {
    const css_rope * n = st.items[--st.len];
    if (work) {
      (*work)++;
    }
    if (n->bytes == 0) {
      continue;
    }
    if (track_taint && (n->flags & FLAG_TAINT_ALL)) {
      /* the range of the whole node: the bytes written are known, whatever its children say */
      if (n->bytes > SIZE_MAX - at || !taint_add(w, at, at + (size_t)n->bytes)) {
        status = w->status != GTEXT_CSS_OK ? w->status : GTEXT_CSS_E_LIMIT_OUTPUT;
        break;
      }
    }
    if (n->kind == KIND_LEAF) {
      if (!emit(w, n->data, n->length)) {
        status = w->status != GTEXT_CSS_OK ? w->status : GTEXT_CSS_E_WRITE;
        break;
      }
      at += n->length;
      continue;
    }
    if ((n->b && !nstack_push(w->alloc, &st, n->b)) || (n->a && !nstack_push(w->alloc, &st, n->a))) {
      status = GTEXT_CSS_E_OOM;
      break;
    }
  }
  gtext_allocator_free((GTEXT_Allocator *)w->alloc, st.items);
  return status;
}

static bool emit_buf(writer * w, const char * bytes, size_t n) {
  if (n > w->max - w->pos) {
    w->status = GTEXT_CSS_E_LIMIT_OUTPUT;
    return false;
  }
  memcpy(w->buf + w->pos, bytes, n);
  w->pos += n;
  return true;
}

static bool emit_sink(writer * w, const char * bytes, size_t n) {
  return w->sink(w->user, bytes, n);
}

GTEXT_CSS_Status css_rope_flatten(const GTEXT_Allocator * alloc, const css_rope * r, size_t max_bytes, char ** out, size_t * length, css_range ** taint,
    size_t * ntaint, size_t * work) {
  const uint64_t total = css_rope_bytes(r);
  if (total > max_bytes || total > SIZE_MAX - 1) {
    return GTEXT_CSS_E_LIMIT_OUTPUT;
  }
  writer w;
  memset(&w, 0, sizeof w);
  w.alloc = alloc;
  w.max = (size_t)total;
  w.buf = gtext_allocator_malloc((GTEXT_Allocator *)alloc, (size_t)total + 1);
  if (!w.buf) {
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (r) {
    st = walk(&w, r, work, emit_buf, taint != NULL && css_rope_has_taint(r));
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free((GTEXT_Allocator *)alloc, w.buf);
    gtext_allocator_free((GTEXT_Allocator *)alloc, w.taint);
    return st;
  }
  w.buf[w.pos] = '\0';
  *out = w.buf;
  *length = w.pos;
  if (taint) {
    *taint = w.taint;
    *ntaint = w.ntaint;
  }
  else {
    gtext_allocator_free((GTEXT_Allocator *)alloc, w.taint);
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_rope_write(const GTEXT_Allocator * alloc, const css_rope * r, css_rope_sink sink, void * user, size_t * work) {
  if (!r || r->bytes == 0) {
    return GTEXT_CSS_OK;
  }
  writer w;
  memset(&w, 0, sizeof w);
  w.alloc = alloc;
  w.sink = sink;
  w.user = user;
  return walk(&w, r, work, emit_sink, false);
}

/* ---- component values ------------------------------------------------------------------------------ */

static css_tokclass delim_class(uint32_t cp) {
  switch (cp) {
  case '#': return CSS_TC_D_HASH;
  case '-': return CSS_TC_D_MINUS;
  case '@': return CSS_TC_D_AT;
  case '.': return CSS_TC_D_DOT;
  case '+': return CSS_TC_D_PLUS;
  case '/': return CSS_TC_D_SLASH;
  case '%': return CSS_TC_D_PCT;
  case '*': return CSS_TC_D_STAR;
  default: return CSS_TC_SAFE;
  }
}

css_tokclass css_cv_first_class(const GTEXT_CSS_Component_Value * cv) {
  if (cv->kind == GTEXT_CSS_VALUE_FUNCTION) {
    return CSS_TC_FUNC;
  }
  if (cv->kind == GTEXT_CSS_VALUE_BLOCK) {
    return cv->type == GTEXT_CSS_TOKEN_OPEN_PAREN ? CSS_TC_LPAREN : CSS_TC_SAFE;
  }
  switch (cv->type) {
  case GTEXT_CSS_TOKEN_IDENT: return CSS_TC_IDENT;
  case GTEXT_CSS_TOKEN_AT_KEYWORD: return CSS_TC_ATKW;
  case GTEXT_CSS_TOKEN_HASH: return CSS_TC_HASH;
  case GTEXT_CSS_TOKEN_NUMBER: return CSS_TC_NUM;
  case GTEXT_CSS_TOKEN_PERCENTAGE: return CSS_TC_PCT;
  case GTEXT_CSS_TOKEN_DIMENSION: return CSS_TC_DIM;
  case GTEXT_CSS_TOKEN_URL: return CSS_TC_URL;
  case GTEXT_CSS_TOKEN_BAD_URL: return CSS_TC_BADURL;
  case GTEXT_CSS_TOKEN_CDC: return CSS_TC_CDC;
  case GTEXT_CSS_TOKEN_DELIM: return delim_class(cv->delim);
  default: return CSS_TC_SAFE;
  }
}

css_tokclass css_cv_last_class(const GTEXT_CSS_Component_Value * cv) {
  if (cv->kind != GTEXT_CSS_VALUE_TOKEN) {
    return CSS_TC_SAFE; /* it ends with a bracket */
  }
  return css_cv_first_class(cv);
}

GTEXT_CSS_Status css_cv_count_tokens(const GTEXT_Allocator * alloc, const GTEXT_CSS_Component_Value * values, size_t count, uint64_t * tokens) {
  typedef struct {
    const GTEXT_CSS_Component_Value * v;
    size_t n, i;
  } fr;
  uint64_t total = 0;
  size_t cap = 16, len = 0;
  fr * st = NULL;
  if (count == 0) {
    *tokens = 0;
    return GTEXT_CSS_OK;
  }
  st = gtext_allocator_malloc((GTEXT_Allocator *)alloc, cap * sizeof *st);
  if (!st) {
    return GTEXT_CSS_E_OOM;
  }
  st[len++] = (fr){values, count, 0};
  while (len) {
    fr * f = &st[len - 1];
    if (f->i >= f->n) {
      len--;
      continue;
    }
    const GTEXT_CSS_Component_Value * v = &f->v[f->i++];
    total = css_sat_add(total, v->kind == GTEXT_CSS_VALUE_TOKEN ? 1 : 2);
    if (v->kind != GTEXT_CSS_VALUE_TOKEN && v->child_count) {
      if (len == cap) {
        fr * g = gtext_allocator_realloc((GTEXT_Allocator *)alloc, st, cap * 2 * sizeof *g);
        if (!g) {
          gtext_allocator_free((GTEXT_Allocator *)alloc, st);
          return GTEXT_CSS_E_OOM;
        }
        st = g;
        cap *= 2;
      }
      st[len++] = (fr){v->children, v->child_count, 0};
    }
  }
  gtext_allocator_free((GTEXT_Allocator *)alloc, st);
  *tokens = total;
  return GTEXT_CSS_OK;
}
