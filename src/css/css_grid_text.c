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
 * `grid-template` and `grid`, as text: the value of the shorthand cut into the texts of its longhands, and the texts of the longhands joined into
 * the value (CSS Grid 2, 7.4 and 7.8). The grid shorthands are the ones whose grammar mixes the rows, the columns and the area names in one
 * list, so they are worked on tokens rather than on a typed value. Nothing here recurses; every function is linear in the text.
 */

#include <string.h>

#include "css_grid_text.h"
#include "css_shorthand_internal.h"

/// One top-level token of a grid value: a bracketed line-name list, a string, the slash, or a word (a keyword or a function with its arguments).
typedef struct {
  const char * p;
  size_t n;
  char kind;   ///< '[' names, '"' string, '/' slash, 'w' word.
} gtok;

typedef struct {
  gtok * v;
  size_t n, cap;
} gtoks;

static bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool push_tok(const GTEXT_Allocator * a, gtoks * t, const char * p, size_t n, char kind) {
  if (t->n == t->cap) {
    const size_t cap = t->cap ? t->cap * 2 : 16;
    gtok * nv = gtext_allocator_realloc(a, t->v, cap * sizeof *nv);
    if (!nv) {
      return false;
    }
    t->v = nv;
    t->cap = cap;
  }
  t->v[t->n].p = p;
  t->v[t->n].n = n;
  t->v[t->n].kind = kind;
  t->n++;
  return true;
}

/// Cut `text` into tokens. False on memory; `*ok` false when the text is not made of the four kinds (an unbalanced quote, bracket or parenthesis).
static bool tokenize(const GTEXT_Allocator * a, const char * text, gtoks * t, bool * ok) {
  memset(t, 0, sizeof *t);
  *ok = true;
  const char * s = text;
  while (*s) {
    if (is_ws(*s)) {
      s++;
      continue;
    }
    const char * start = s;
    char kind = 'w';
    if (*s == '/') {
      s++;
      kind = '/';
    }
    else if (*s == '[') {
      kind = '[';
      while (*s && *s != ']') {
        s++;
      }
      if (*s != ']') {
        *ok = false;
        return true;
      }
      s++;
    }
    else if (*s == '"' || *s == '\'') {
      kind = '"';
      const char q = *s++;
      while (*s && *s != q) {
        s += (*s == '\\' && s[1]) ? 2 : 1;
      }
      if (*s != q) {
        *ok = false;
        return true;
      }
      s++;
    }
    else {
      int depth = 0;
      while (*s && (depth > 0 || (!is_ws(*s) && *s != '/' && *s != '['))) {
        if (*s == '(') {
          depth++;
        }
        else if (*s == ')') {
          depth--;
        }
        else if (*s == '"' || *s == '\'') {
          const char q = *s++;
          while (*s && *s != q) {
            s += (*s == '\\' && s[1]) ? 2 : 1;
          }
          if (!*s) {
            *ok = false;
            return true;
          }
        }
        s++;
      }
      if (depth != 0) {
        *ok = false;
        return true;
      }
    }
    if (!push_tok(a, t, start, (size_t)(s - start), kind)) {
      return false;
    }
  }
  return true;
}

static void toks_free(const GTEXT_Allocator * a, gtoks * t) {
  gtext_allocator_free(a, t->v);
  memset(t, 0, sizeof *t);
}

static bool word_is(const gtok * t, const char * w) {
  const size_t n = strlen(w);
  return t->kind == 'w' && t->n == n && memcmp(t->p, w, n) == 0;
}

/// Write tokens [from, to) separated by single spaces. A bracketed list is rewritten with single spaces; an empty one is dropped.
static void put_toks(tbuf * b, const gtok * v, size_t from, size_t to) {
  bool first = true;
  for (size_t i = from; i < to; i++) {
    if (v[i].kind == '[') {
      /* names only: collapse the white space */
      tbuf names;
      tb_init(&names, &b->alloc);
      bool any = false;
      size_t k = 1;
      while (k + 1 < v[i].n) {
        while (k + 1 < v[i].n && is_ws(v[i].p[k])) {
          k++;
        }
        const size_t s = k;
        while (k + 1 < v[i].n && !is_ws(v[i].p[k])) {
          k++;
        }
        if (k > s) {
          if (any) {
            tb_put(&names, " ", 1);
          }
          tb_put(&names, v[i].p + s, k - s);
          any = true;
        }
      }
      if (any) {
        if (!first) {
          tb_put(b, " ", 1);
        }
        tb_put(b, "[", 1);
        tb_put(b, names.data, names.len);
        tb_put(b, "]", 1);
        first = false;
      }
      tb_free(&names);
      continue;
    }
    if (!first) {
      tb_put(b, " ", 1);
    }
    tb_put(b, v[i].p, v[i].n);
    first = false;
  }
}

static char * take_or_none(tbuf * b) {
  if (b->len == 0 && !b->oom) {
    tb_puts(b, "none");
  }
  return tb_take(b, NULL);
}

/* ---- grid-template: the value to the longhands ------------------------------------------------------------------------------------- */

/// Split the tokens [from, to) of a `grid-template` value (rows, columns, areas into out[0..2]); out[] are new strings.
static css_grid_status split_template_toks(const GTEXT_Allocator * a, const gtoks * t, size_t from, size_t to, char * out[3]) {
  size_t slash = to;
  bool strings = false;
  for (size_t i = from; i < to; i++) {
    if (t->v[i].kind == '/' && slash == to) {
      slash = i;
    }
    if (t->v[i].kind == '"' && slash == to) {
      strings = true;
    }
  }
  tbuf rows, cols, areas;
  tb_init(&rows, a);
  tb_init(&cols, a);
  tb_init(&areas, a);
  if (!strings) {
    if (slash == to) {
      /* a lone `none` (or one track list: not a value of this shorthand) */
      if (to - from == 1 && word_is(&t->v[from], "none")) {
        tb_puts(&rows, "none");
        tb_puts(&cols, "none");
      }
      else {
        tb_free(&rows);
        tb_free(&cols);
        tb_free(&areas);
        return CSS_GRID_UNSUPPORTED;
      }
    }
    else {
      put_toks(&rows, t->v, from, slash);
      put_toks(&cols, t->v, slash + 1, to);
    }
    tb_puts(&areas, "none");
  }
  else {
    /* [ <line-names>? <string> <track-size>? <line-names>? ]+ [ / <explicit-track-list> ]? */
    tbuf pend;
    tb_init(&pend, a);
    bool first_row = true;
    size_t i = from;
    bool first_str = true;
    while (i < slash) {
      const gtok * k = &t->v[i];
      if (k->kind == '[') {
        gtok one = *k;
        tbuf tmp;
        tb_init(&tmp, a);
        put_toks(&tmp, &one, 0, 1);   /* "[a b]" or nothing */
        if (tmp.len > 2) {
          if (pend.len) {
            pend.len--;               /* drop the closing bracket of the group before */
            tb_put(&pend, " ", 1);
            tb_put(&pend, tmp.data + 1, tmp.len - 1);
          }
          else {
            tb_put(&pend, tmp.data, tmp.len);
          }
        }
        tb_free(&tmp);
        i++;
      }
      else if (k->kind == '"') {
        if (pend.len) {
          if (!first_row) {
            tb_put(&rows, " ", 1);
          }
          tb_put(&rows, pend.data, pend.len);
          first_row = false;
          pend.len = 0;
        }
        if (!first_str) {
          tb_put(&areas, " ", 1);
        }
        tb_put(&areas, k->p, k->n);
        first_str = false;
        i++;
        const char * size = "auto";
        size_t size_n = 4;
        if (i < slash && t->v[i].kind == 'w') {
          size = t->v[i].p;
          size_n = t->v[i].n;
          i++;
        }
        if (!first_row) {
          tb_put(&rows, " ", 1);
        }
        tb_put(&rows, size, size_n);
        first_row = false;
      }
      else {
        tb_free(&pend);
        tb_free(&rows);
        tb_free(&cols);
        tb_free(&areas);
        return CSS_GRID_UNSUPPORTED;
      }
    }
    if (pend.len) {
      if (!first_row) {
        tb_put(&rows, " ", 1);
      }
      tb_put(&rows, pend.data, pend.len);
    }
    tb_free(&pend);
    if (slash != to) {
      put_toks(&cols, t->v, slash + 1, to);
    }
  }
  if (rows.oom || cols.oom || areas.oom) {
    tb_free(&rows);
    tb_free(&cols);
    tb_free(&areas);
    return CSS_GRID_OOM;
  }
  out[0] = take_or_none(&rows);
  out[1] = take_or_none(&cols);
  out[2] = tb_take(&areas, NULL);
  if (!out[0] || !out[1] || !out[2]) {
    for (int k = 0; k < 3; k++) {
      gtext_allocator_free(a, out[k]);
      out[k] = NULL;
    }
    return CSS_GRID_OOM;
  }
  return CSS_GRID_OK;
}

static css_grid_status template_split_impl(const GTEXT_Allocator * a, const char * text, char * out[3]) {
  gtoks t;
  bool ok;
  if (!tokenize(a, text, &t, &ok)) {
    return CSS_GRID_OOM;
  }
  css_grid_status st = CSS_GRID_UNSUPPORTED;
  if (ok) {
    st = split_template_toks(a, &t, 0, t.n, out);
  }
  toks_free(a, &t);
  return st;
}

/* ---- grid ------------------------------------------------------------------------------------------------------------------------ */

/// The `[ auto-flow && dense? ]` part and the track sizes after it, of tokens [from, to). `dense` and the rest (a new string, "auto" when empty).
static bool take_auto_flow(const GTEXT_Allocator * a, const gtoks * t, size_t from, size_t to, bool * dense, char ** sizes, bool * oom) {
  bool flow = false;
  *dense = false;
  tbuf b;
  tb_init(&b, a);
  size_t rest_from = from;
  for (size_t i = from; i < to && i < from + 2; i++) {
    if (word_is(&t->v[i], "auto-flow") && !flow) {
      flow = true;
      rest_from = i + 1;
    }
    else if (word_is(&t->v[i], "dense") && !*dense) {
      *dense = true;
      rest_from = i + 1;
    }
    else {
      break;
    }
  }
  if (!flow) {
    tb_free(&b);
    return false;
  }
  put_toks(&b, t->v, rest_from, to);
  if (b.len == 0) {
    tb_puts(&b, "auto");
  }
  *oom = b.oom;
  *sizes = tb_take(&b, NULL);
  if (!*sizes) {
    *oom = true;
  }
  return true;
}

static bool starts_auto_flow(const gtoks * t, size_t from, size_t to) {
  for (size_t i = from; i < to && i < from + 2; i++) {
    if (word_is(&t->v[i], "auto-flow")) {
      return true;
    }
    if (!word_is(&t->v[i], "dense")) {
      return false;
    }
  }
  return false;
}

static char * dupz(const GTEXT_Allocator * a, const char * s) {
  tbuf b;
  tb_init(&b, a);
  tb_puts(&b, s);
  return tb_take(&b, NULL);
}

static css_grid_status split_impl(const GTEXT_Allocator * a, const char * text, char * out[6]) {
  gtoks t;
  bool ok;
  if (!tokenize(a, text, &t, &ok)) {
    return CSS_GRID_OOM;
  }
  css_grid_status st = CSS_GRID_UNSUPPORTED;
  for (int i = 0; i < 6; i++) {
    out[i] = NULL;
  }
  if (ok) {
    size_t slash = t.n;
    for (size_t i = 0; i < t.n; i++) {
      if (t.v[i].kind == '/') {
        slash = i;
        break;
      }
    }
    const bool left_flow = slash < t.n && starts_auto_flow(&t, 0, slash);
    const bool right_flow = slash < t.n && starts_auto_flow(&t, slash + 1, t.n);
    if (left_flow || right_flow) {
      bool dense = false, oom = false;
      char * sizes = NULL;
      tbuf other;
      tb_init(&other, a);
      if (left_flow) {
        if (take_auto_flow(a, &t, 0, slash, &dense, &sizes, &oom) && !oom) {
          put_toks(&other, t.v, slash + 1, t.n);
          out[0] = dupz(a, "none");
          out[1] = tb_take(&other, NULL);
          out[2] = dupz(a, "none");
          out[3] = sizes;
          out[4] = dupz(a, "auto");
          out[5] = dupz(a, dense ? "row dense" : "row");
          st = CSS_GRID_OK;
        }
      }
      else if (take_auto_flow(a, &t, slash + 1, t.n, &dense, &sizes, &oom) && !oom) {
        put_toks(&other, t.v, 0, slash);
        out[0] = tb_take(&other, NULL);
        out[1] = dupz(a, "none");
        out[2] = dupz(a, "none");
        out[3] = dupz(a, "auto");
        out[4] = sizes;
        out[5] = dupz(a, dense ? "column dense" : "column");
        st = CSS_GRID_OK;
      }
      if (oom) {
        st = CSS_GRID_OOM;
        gtext_allocator_free(a, sizes);
      }
      tb_free(&other);
    }
    else {
      char * tpl[3] = {NULL, NULL, NULL};
      st = split_template_toks(a, &t, 0, t.n, tpl);
      if (st == CSS_GRID_OK) {
        out[0] = tpl[0];
        out[1] = tpl[1];
        out[2] = tpl[2];
        out[3] = dupz(a, "auto");
        out[4] = dupz(a, "auto");
        out[5] = dupz(a, "row");
      }
    }
    if (st == CSS_GRID_OK) {
      for (int i = 0; i < 6; i++) {
        if (!out[i]) {
          st = CSS_GRID_OOM;
        }
      }
    }
    if (st != CSS_GRID_OK) {
      for (int i = 0; i < 6; i++) {
        gtext_allocator_free(a, out[i]);
        out[i] = NULL;
      }
    }
  }
  toks_free(a, &t);
  return st;
}

/* ---- the longhands to the value ------------------------------------------------------------------------------------------------------ */

static bool text_is(const char * s, const char * w) {
  return s && strcmp(s, w) == 0;
}

/// Join rows, columns and areas into a `grid-template` value. `*text` is NULL when they make none (not an error).
static css_grid_status join_template(const GTEXT_Allocator * a, const char * rows, const char * cols, const char * areas, char ** text) {
  *text = NULL;
  tbuf b;
  tb_init(&b, a);
  if (text_is(areas, "none")) {
    if (text_is(rows, "none") && text_is(cols, "none")) {
      tb_puts(&b, "none");
    }
    else {
      tb_puts(&b, rows);
      tb_puts(&b, " / ");
      tb_puts(&b, cols);
    }
    *text = tb_take(&b, NULL);
    return *text ? CSS_GRID_OK : CSS_GRID_OOM;
  }
  /* the areas form: the rows must be explicit tracks, one for each string, with no repeat() and no subgrid */
  gtoks rt, at;
  bool ok1, ok2;
  if (!tokenize(a, rows, &rt, &ok1)) {
    tb_free(&b);
    return CSS_GRID_OOM;
  }
  if (!tokenize(a, areas, &at, &ok2)) {
    toks_free(a, &rt);
    tb_free(&b);
    return CSS_GRID_OOM;
  }
  bool fits = ok1 && ok2;
  size_t ntracks = 0;
  for (size_t i = 0; fits && i < rt.n; i++) {
    if (rt.v[i].kind == 'w') {
      ntracks++;
      if ((rt.v[i].n >= 7 && memcmp(rt.v[i].p, "repeat(", 7) == 0) || word_is(&rt.v[i], "subgrid") || word_is(&rt.v[i], "none")) {
        fits = false;
      }
    }
    else if (rt.v[i].kind != '[') {
      fits = false;
    }
  }
  size_t nstr = 0;
  for (size_t i = 0; fits && i < at.n; i++) {
    if (at.v[i].kind != '"') {
      fits = false;
    }
    nstr++;
  }
  if (fits && (nstr == 0 || nstr != ntracks)) {
    fits = false;
  }
  if (fits) {
    /* boundary groups: the line names before the first track, between two, and after the last */
    size_t ri = 0, si = 0;
    bool first = true;
    for (size_t boundary = 0; boundary <= ntracks; boundary++) {
      tbuf names;
      tb_init(&names, a);
      bool any = false;
      while (ri < rt.n && rt.v[ri].kind == '[') {
        gtok one = rt.v[ri];
        tbuf tmp;
        tb_init(&tmp, a);
        put_toks(&tmp, &one, 0, 1);
        if (tmp.len > 2) {
          if (any) {
            tb_put(&names, " ", 1);
          }
          tb_put(&names, tmp.data + 1, tmp.len - 2);
          any = true;
        }
        tb_free(&tmp);
        ri++;
      }
      if (any) {
        if (!first) {
          tb_put(&b, " ", 1);
        }
        tb_put(&b, "[", 1);
        tb_put(&b, names.data, names.len);
        tb_put(&b, "]", 1);
        first = false;
      }
      tb_free(&names);
      if (boundary == ntracks) {
        break;
      }
      /* the track: its string, then its size unless it is auto */
      if (!first) {
        tb_put(&b, " ", 1);
      }
      tb_put(&b, at.v[si].p, at.v[si].n);
      first = false;
      si++;
      if (!word_is(&rt.v[ri], "auto")) {
        tb_put(&b, " ", 1);
        tb_put(&b, rt.v[ri].p, rt.v[ri].n);
      }
      ri++;
    }
    if (!text_is(cols, "none")) {
      tb_puts(&b, " / ");
      tb_puts(&b, cols);
    }
  }
  toks_free(a, &rt);
  toks_free(a, &at);
  if (b.oom) {
    tb_free(&b);
    return CSS_GRID_OOM;
  }
  if (!fits) {
    tb_free(&b);
    return CSS_GRID_OK;
  }
  *text = tb_take(&b, NULL);
  return *text ? CSS_GRID_OK : CSS_GRID_OOM;
}

static css_grid_status template_join_impl(const GTEXT_Allocator * a, const char * const texts[3], char ** text) {
  return join_template(a, texts[0], texts[1], texts[2], text);
}

static css_grid_status join_impl(const GTEXT_Allocator * a, const char * const texts[6], char ** text) {
  *text = NULL;
  const char * rows = texts[0];
  const char * cols = texts[1];
  const char * areas = texts[2];
  const char * arows = texts[3];
  const char * acols = texts[4];
  const char * flow = texts[5];
  if (text_is(arows, "auto") && text_is(acols, "auto") && text_is(flow, "row")) {
    return join_template(a, rows, cols, areas, text);
  }
  if (!text_is(areas, "none")) {
    return CSS_GRID_OK;
  }
  bool column = false, dense = false, other = false;
  gtoks ft;
  bool ok;
  if (!tokenize(a, flow, &ft, &ok)) {
    return CSS_GRID_OOM;
  }
  for (size_t i = 0; i < ft.n; i++) {
    if (word_is(&ft.v[i], "column")) {
      column = true;
    }
    else if (word_is(&ft.v[i], "dense")) {
      dense = true;
    }
    else if (!word_is(&ft.v[i], "row")) {
      other = true;
    }
  }
  toks_free(a, &ft);
  if (other || !ok) {
    return CSS_GRID_OK;
  }
  tbuf b;
  tb_init(&b, a);
  if (column) {
    if (!text_is(cols, "none") || !text_is(arows, "auto")) {
      tb_free(&b);
      return CSS_GRID_OK;
    }
    tb_puts(&b, rows);
    tb_puts(&b, " / auto-flow");
    if (dense) {
      tb_puts(&b, " dense");
    }
    if (!text_is(acols, "auto")) {
      tb_puts(&b, " ");
      tb_puts(&b, acols);
    }
  }
  else {
    if (!text_is(rows, "none") || !text_is(acols, "auto")) {
      tb_free(&b);
      return CSS_GRID_OK;
    }
    tb_puts(&b, "auto-flow");
    if (dense) {
      tb_puts(&b, " dense");
    }
    if (!text_is(arows, "auto")) {
      tb_puts(&b, " ");
      tb_puts(&b, arows);
    }
    tb_puts(&b, " / ");
    tb_puts(&b, cols);
  }
  if (b.oom) {
    tb_free(&b);
    return CSS_GRID_OOM;
  }
  *text = tb_take(&b, NULL);
  return *text ? CSS_GRID_OK : CSS_GRID_OOM;
}

/* ---- the entry points: an allocation that fails anywhere is a failure of the call ---------------------------------------------------------- */

/* The work above puts text in buffers that remember a failure and do not stop; one that was dropped on the way would give a shorter text that
 * is not the value's. So the allocator the work uses is a shim that records any failure, and the call is OOM if there was one, whatever else it
 * came to. The strings returned are plain heap memory, freed with the caller's own allocator. */
typedef struct {
  GTEXT_Allocator base;
  GTEXT_Allocator alloc;
  bool failed;
} gfail;

static void * gf_malloc(void * ctx, size_t n) {
  gfail * g = ctx;
  void * p = gtext_allocator_malloc(&g->base, n);
  g->failed = g->failed || !p;
  return p;
}

static void * gf_calloc(void * ctx, size_t n, size_t size) {
  gfail * g = ctx;
  void * p = gtext_allocator_calloc(&g->base, n, size);
  g->failed = g->failed || !p;
  return p;
}

static void * gf_realloc(void * ctx, void * old, size_t n) {
  gfail * g = ctx;
  void * p = gtext_allocator_realloc(&g->base, old, n);
  g->failed = g->failed || !p;
  return p;
}

static void gf_free(void * ctx, void * p) {
  gfail * g = ctx;
  gtext_allocator_free(&g->base, p);
}

static void gfail_init(gfail * g, const GTEXT_Allocator * a) {
  g->base = *a;
  g->failed = false;
  g->alloc.ctx = g;
  g->alloc.malloc_fn = gf_malloc;
  g->alloc.calloc_fn = gf_calloc;
  g->alloc.realloc_fn = gf_realloc;
  g->alloc.free_fn = gf_free;
}

css_grid_status css_grid_template_split(const GTEXT_Allocator * a, const char * text, char * out[3]) {
  gfail g;
  gfail_init(&g, a);
  css_grid_status st = template_split_impl(&g.alloc, text, out);
  if (g.failed && st != CSS_GRID_OOM) {
    for (int i = 0; i < 3; i++) {
      gtext_allocator_free(a, out[i]);
      out[i] = NULL;
    }
    st = CSS_GRID_OOM;
  }
  return st;
}

css_grid_status css_grid_split(const GTEXT_Allocator * a, const char * text, char * out[6]) {
  gfail g;
  gfail_init(&g, a);
  css_grid_status st = split_impl(&g.alloc, text, out);
  if (g.failed && st != CSS_GRID_OOM) {
    for (int i = 0; i < 6; i++) {
      gtext_allocator_free(a, out[i]);
      out[i] = NULL;
    }
    st = CSS_GRID_OOM;
  }
  return st;
}

css_grid_status css_grid_template_join(const GTEXT_Allocator * a, const char * const texts[3], char ** text) {
  gfail g;
  gfail_init(&g, a);
  css_grid_status st = template_join_impl(&g.alloc, texts, text);
  if (g.failed && st != CSS_GRID_OOM) {
    gtext_allocator_free(a, *text);
    *text = NULL;
    st = CSS_GRID_OOM;
  }
  return st;
}

css_grid_status css_grid_join(const GTEXT_Allocator * a, const char * const texts[6], char ** text) {
  gfail g;
  gfail_init(&g, a);
  css_grid_status st = join_impl(&g.alloc, texts, text);
  if (g.failed && st != CSS_GRID_OOM) {
    gtext_allocator_free(a, *text);
    *text = NULL;
    st = CSS_GRID_OOM;
  }
  return st;
}
