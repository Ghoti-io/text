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
 * Serializing a calculation (Values 4, 10.13, "serialize a math function" and
 * "serialize a calculation tree"), with an explicit stack: a tree as deep as the
 * limit allows is written without recursion.
 *
 * An operator node is written in parentheses; a function's arguments are
 * written without the outer ones (the draft strips them from the text of each
 * argument, which is the same as not writing them). The children of a sum or a
 * product are ordered as the draft says: the number, the percentage, the
 * dimensions by unit (ASCII case-insensitively), and the rest as they were.
 */

#include <float.h>
#include <string.h>

#include "css_math_internal.h"

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap, max;
  GTEXT_CSS_Status status;
} obuf;

static bool put(obuf * b, const char * s, size_t n) {
  if (b->status != GTEXT_CSS_OK) {
    return false;
  }
  if (n > b->max - b->len) {
    b->status = GTEXT_CSS_E_LIMIT_OUTPUT;
    return false;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char * p = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!p) {
      b->status = GTEXT_CSS_E_OOM;
      return false;
    }
    b->data = p;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return true;
}

static bool puts_(obuf * b, const char * s) {
  return put(b, s, strlen(s));
}

static bool is_finite_(double v) {
  return v == v && v <= DBL_MAX && v >= -DBL_MAX;
}

static const char * keyword_of(double v) {
  if (v != v) {
    return "NaN";
  }
  return v > 0 ? "infinity" : "-infinity";
}

static const char * canonical_unit_text(GTEXT_CSS_Math_Category cat) {
  switch (cat) {
  case GTEXT_CSS_MATH_CAT_PERCENTAGE: return "%";
  case GTEXT_CSS_MATH_CAT_NUMBER: return NULL;
  default: return gtext_css_unit_name(css_math_category_unit(cat));
  }
}

static bool put_number(obuf * b, double v) {
  char t[340];
  const size_t n = css_cssom_number(v, false, t);
  return put(b, t, n);
}

/// A numeric leaf: `12px`, `5%`, `3`, or when it is infinite or NaN `infinity * 1px`.
static bool put_leaf(obuf * b, const mnode * n, bool paren) {
  if (!is_finite_(n->value)) {
    if (paren && !puts_(b, "(")) {
      return false;
    }
    if (!puts_(b, keyword_of(n->value))) {
      return false;
    }
    if (n->op == GTEXT_CSS_MATH_PERCENT) {
      if (!puts_(b, " * 1%")) {
        return false;
      }
    }
    else if (n->op == GTEXT_CSS_MATH_DIMENSION) {
      const char * u = gtext_css_unit_name(n->unit);
      if (!puts_(b, " * 1") || !puts_(b, u)) {
        return false;
      }
    }
    return !paren || puts_(b, ")");
  }
  if (!put_number(b, n->value)) {
    return false;
  }
  if (n->op == GTEXT_CSS_MATH_PERCENT) {
    return puts_(b, "%");
  }
  if (n->op == GTEXT_CSS_MATH_DIMENSION) {
    return puts_(b, gtext_css_unit_name(n->unit));
  }
  return true;
}

static int ascii_icmp(const char * a, const char * b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') {
      x = (char)(x + 32);
    }
    if (y >= 'A' && y <= 'Z') {
      y = (char)(y + 32);
    }
    if (x != y) {
      return (unsigned char)x < (unsigned char)y ? -1 : 1;
    }
    if (!x) {
      return 0;
    }
  }
}

/// The order of a sum's or a product's children: number, percentage, dimensions by unit, the rest.
static size_t * child_order(obuf * b, const mnode * n) {
  size_t * order = gtext_allocator_malloc(&b->alloc, (n->kid_count ? n->kid_count : 1) * sizeof *order);
  if (!order) {
    b->status = GTEXT_CSS_E_OOM;
    return NULL;
  }
  size_t m = 0;
  for (size_t i = 0; i < n->kid_count; i++) {
    if (n->kids[i]->op == GTEXT_CSS_MATH_NUMBER) {
      order[m++] = i;
    }
  }
  for (size_t i = 0; i < n->kid_count; i++) {
    if (n->kids[i]->op == GTEXT_CSS_MATH_PERCENT) {
      order[m++] = i;
    }
  }
  const size_t dim_start = m;
  for (size_t i = 0; i < n->kid_count; i++) {
    if (n->kids[i]->op == GTEXT_CSS_MATH_DIMENSION) {
      /* insertion sort by unit name, stable */
      size_t j = m++;
      while (j > dim_start && ascii_icmp(gtext_css_unit_name(n->kids[order[j - 1]]->unit), gtext_css_unit_name(n->kids[i]->unit)) > 0) {
        order[j] = order[j - 1];
        j--;
      }
      order[j] = i;
    }
  }
  for (size_t i = 0; i < n->kid_count; i++) {
    const GTEXT_CSS_Math_Op op = n->kids[i]->op;
    if (op != GTEXT_CSS_MATH_NUMBER && op != GTEXT_CSS_MATH_PERCENT && op != GTEXT_CSS_MATH_DIMENSION) {
      order[m++] = i;
    }
  }
  return order;
}

typedef struct {
  const mnode * n;
  size_t idx;
  size_t * order;
  bool paren;
} sframe;

typedef struct {
  sframe * v;
  size_t n, cap;
} sstack;

static const char * strategy_text(GTEXT_CSS_Math_Round r) {
  switch (r) {
  case GTEXT_CSS_MATH_ROUND_UP: return "up";
  case GTEXT_CSS_MATH_ROUND_DOWN: return "down";
  case GTEXT_CSS_MATH_ROUND_TO_ZERO: return "to-zero";
  case GTEXT_CSS_MATH_ROUND_LINE_WIDTH: return "line-width";
  default: return "nearest";
  }
}

/// Write `n`, or open a frame for it. `paren`: an operator node is written in parentheses.
static bool start(obuf * b, sstack * st, const mnode * n, bool paren) {
  switch (n->op) {
  case GTEXT_CSS_MATH_NUMBER: case GTEXT_CSS_MATH_DIMENSION: case GTEXT_CSS_MATH_PERCENT:
    return put_leaf(b, n, paren);
  case GTEXT_CSS_MATH_SYMBOL: case GTEXT_CSS_MATH_UNRESOLVED:
    return put(b, n->text.data, n->text.length);
  default:
    break;
  }
  if (st->n == st->cap) {
    const size_t cap = st->cap ? st->cap * 2 : 32;
    sframe * p = gtext_allocator_realloc(&b->alloc, st->v, cap * sizeof *p);
    if (!p) {
      b->status = GTEXT_CSS_E_OOM;
      return false;
    }
    st->v = p;
    st->cap = cap;
  }
  sframe * f = &st->v[st->n];
  f->n = n;
  f->idx = 0;
  f->order = NULL;
  f->paren = paren;
  switch (n->op) {
  case GTEXT_CSS_MATH_SUM: case GTEXT_CSS_MATH_PRODUCT:
    f->order = child_order(b, n);
    if (!f->order || (paren && !puts_(b, "("))) {
      gtext_allocator_free(&b->alloc, f->order);
      return false;
    }
    break;
  case GTEXT_CSS_MATH_NEGATE:
    if (!puts_(b, paren ? "(-1 * " : "-1 * ")) {
      return false;
    }
    break;
  case GTEXT_CSS_MATH_INVERT:
    if (!puts_(b, paren ? "(1 / " : "1 / ")) {
      return false;
    }
    break;
  default:
    if (!puts_(b, gtext_css_math_op_name(n->op)) || !puts_(b, "(")) {
      return false;
    }
    if (n->op == GTEXT_CSS_MATH_ROUND && n->strategy_written) {
      if (!puts_(b, strategy_text(n->round)) || !puts_(b, ", ")) {
        return false;
      }
    }
    if (n->op == GTEXT_CSS_MATH_PROGRESS && n->strategy_written && !puts_(b, "no-clamp ")) {
      return false;
    }
    if (n->op == GTEXT_CSS_MATH_RANDOM && n->text.length && (!put(b, n->text.data, n->text.length) || !puts_(b, ", "))) {
      return false;
    }
    break;
  }
  st->n++;
  return true;
}

static bool is_negative_leaf(const mnode * n) {
  return css_math_is_numeric(n) && n->value < 0.0;
}

static bool write_tree(obuf * b, const mnode * root, bool paren) {
  sstack st = {NULL, 0, 0};
  bool ok = start(b, &st, root, paren);
  while (ok && st.n > 0) {
    sframe * f = &st.v[st.n - 1];
    const mnode * n = f->n;
    switch (n->op) {
    case GTEXT_CSS_MATH_SUM: {
      if (f->idx >= n->kid_count) {
        gtext_allocator_free(&b->alloc, f->order);
        ok = !f->paren || puts_(b, ")");
        st.n--;
        continue;
      }
      const mnode * c = n->kids[f->order[f->idx]];
      const bool first = f->idx == 0;
      f->idx++;
      if (first) {
        ok = start(b, &st, c, true);
      }
      else if (c->op == GTEXT_CSS_MATH_NEGATE) {
        ok = puts_(b, " - ") && start(b, &st, c->kids[0], true);
      }
      else if (is_negative_leaf(c)) {
        mnode neg = *c;
        neg.value = -c->value;
        ok = puts_(b, " - ") && put_leaf(b, &neg, true);
      }
      else if (css_math_is_numeric(c) && !is_finite_(c->value) && c->value < 0.0) {
        mnode neg = *c;
        neg.value = -c->value;
        ok = puts_(b, " - ") && put_leaf(b, &neg, true);
      }
      else {
        ok = puts_(b, " + ") && start(b, &st, c, true);
      }
      continue;
    }
    case GTEXT_CSS_MATH_PRODUCT: {
      if (f->idx >= n->kid_count) {
        gtext_allocator_free(&b->alloc, f->order);
        ok = !f->paren || puts_(b, ")");
        st.n--;
        continue;
      }
      const mnode * c = n->kids[f->order[f->idx]];
      const bool first = f->idx == 0;
      f->idx++;
      if (first) {
        ok = start(b, &st, c, true);
      }
      else if (c->op == GTEXT_CSS_MATH_INVERT) {
        ok = puts_(b, " / ") && start(b, &st, c->kids[0], true);
      }
      else {
        ok = puts_(b, " * ") && start(b, &st, c, true);
      }
      continue;
    }
    case GTEXT_CSS_MATH_NEGATE: case GTEXT_CSS_MATH_INVERT:
      if (f->idx == 0) {
        f->idx = 1;
        ok = start(b, &st, n->kids[0], true);
      }
      else {
        ok = !f->paren || puts_(b, ")");
        st.n--;
      }
      continue;
    default:
      if (f->idx >= n->kid_count) {
        ok = puts_(b, ")");
        st.n--;
        continue;
      }
      {
        const mnode * c = n->kids[f->idx];
        const bool first = f->idx == 0;
        f->idx++;
        ok = (first || puts_(b, ", ")) && start(b, &st, c, false);
      }
      continue;
    }
  }
  for (size_t i = 0; i < st.n; i++) {
    gtext_allocator_free(&b->alloc, st.v[i].order);
  }
  gtext_allocator_free(&b->alloc, st.v);
  return ok && b->status == GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_math_serialize_to(const GTEXT_CSS_Math * math, const GTEXT_Allocator * alloc, size_t max_bytes,
    char ** out, size_t * out_len) {
  obuf b;
  memset(&b, 0, sizeof b);
  b.alloc = *alloc;
  b.max = max_bytes;
  b.status = GTEXT_CSS_OK;
  const mnode * root = math->root;
  bool ok;
  if (css_math_is_numeric(root) && !is_finite_(root->value)) {
    ok = puts_(&b, "calc(") && puts_(&b, keyword_of(root->value));
    const char * u = canonical_unit_text(css_math_type_category(&math->type));
    if (ok && u) {
      ok = puts_(&b, " * 1") && puts_(&b, u);
    }
    ok = ok && puts_(&b, ")");
  }
  else if (root->op == GTEXT_CSS_MATH_UNRESOLVED) {
    ok = put(&b, root->text.data, root->text.length);
  }
  else if (css_math_is_numeric(root) || root->op == GTEXT_CSS_MATH_SYMBOL) {
    ok = puts_(&b, "calc(") && write_tree(&b, root, false) && puts_(&b, ")");
  }
  else if (root->op == GTEXT_CSS_MATH_SUM || root->op == GTEXT_CSS_MATH_PRODUCT || root->op == GTEXT_CSS_MATH_NEGATE ||
           root->op == GTEXT_CSS_MATH_INVERT) {
    ok = puts_(&b, "calc(") && write_tree(&b, root, false) && puts_(&b, ")");
  }
  else {
    ok = write_tree(&b, root, false);
  }
  if (!ok) {
    gtext_allocator_free(&b.alloc, b.data);
    return b.status == GTEXT_CSS_OK ? GTEXT_CSS_E_INTERNAL : b.status;
  }
  if (!b.data) {
    b.data = gtext_allocator_malloc(&b.alloc, 1);
    if (!b.data) {
      return GTEXT_CSS_E_OOM;
    }
    b.data[0] = '\0';
  }
  *out = b.data;
  *out_len = b.len;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_math_serialize_buffer(const GTEXT_CSS_Math * math, const GTEXT_CSS_Serialize_Options * options,
    char ** out, size_t * out_length) {
  if (!math || !math->root || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * a = options && options->allocator ? options->allocator : gtext_allocator_default();
  const size_t max = options && options->max_output_bytes ? options->max_output_bytes : (size_t)256 << 20;
  return css_math_serialize_to(math, a, max, out, out_length);
}

GTEXT_CSS_Status gtext_css_math_serialize(const GTEXT_CSS_Math * math, const GTEXT_CSS_Serialize_Options * options,
    const GTEXT_Markup_Sink * sink) {
  if (!sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  char * buf = NULL;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_math_serialize_buffer(math, options, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const bool wrote = sink->write(sink->user, buf, len) == GTEXT_MARKUP_OK;
  const GTEXT_Allocator * a = options && options->allocator ? options->allocator : gtext_allocator_default();
  gtext_allocator_free(a, buf);
  return wrote ? GTEXT_CSS_OK : GTEXT_CSS_E_WRITE;
}
