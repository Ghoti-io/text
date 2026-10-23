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
 * Easing functions (CSS Easing Functions 1 and 2).
 *
 * Reference: CSS Easing Functions Level 2, Editor's Draft of 23 February 2026 (served 2026-10-22); Level 1.
 *
 * The grammars are the drafts' own (Value Definition Syntax), retrieved 2026-10-22; where the
 * served draft differs from an older text the served one is followed and the difference is noted
 * beside the entry.
 * **Implicitly omitted commas are written out.** Values 4, 2.2 omits a comma from the match when the terms around it are omitted
 * (`steps(4)` for `steps( <integer> , <step-position>? )`). The engine keeps a literal comma that separates a possibly empty term
 * as a token, so the grammars here say it with a group (`steps( <integer> [ , <step-position> ]? )`), which matches the same inputs.
 *
 * They are matched by the Value Definition Syntax engine; nothing here is code.
 */

#include "css_function_internal.h"

static const css_fn_type TYPES[] = {
  /* Easing 2, 2 */
  {"easing-function", GTEXT_CSS_REF_TYPE,
      "<linear-easing-function> | <cubic-bezier-easing-function> | <step-easing-function>"},
  {"linear-easing-function", GTEXT_CSS_REF_TYPE,
      "linear | <linear()>"},
  {"linear()", GTEXT_CSS_REF_TYPE,
      "linear( [ <number> && <percentage>{0,2} ]# )"},
  {"cubic-bezier-easing-function", GTEXT_CSS_REF_TYPE,
      "ease | ease-in | ease-out | ease-in-out | <cubic-bezier()>"},
  /* the x values are in [0,1] by the range of the grammar */
  {"cubic-bezier()", GTEXT_CSS_REF_TYPE,
      "cubic-bezier( [ <number [0,1]> , <number> ]#{2} )"},
  {"step-easing-function", GTEXT_CSS_REF_TYPE,
      "step-start | step-end | <steps()>"},
  /* the count's own range (at least 1, at least 2 for jump-none) is a post-parse check, gtext_css_easing_from_value() */
  {"steps()", GTEXT_CSS_REF_TYPE,
      "steps( <integer> [ , <step-position> ]? )"},
  {"step-position", GTEXT_CSS_REF_TYPE,
      "jump-start | jump-end | jump-none | jump-both | start | end"},
};

const css_fn_type * css_easing_types(size_t * count) {
  *count = sizeof TYPES / sizeof TYPES[0];
  return TYPES;
}

/* ---- the typed view ---------------------------------------------------------------- */

#include <string.h>

#include "css_internal.h"

static bool number_of(const GTEXT_CSS_Value * v, double * out) {
  return css_fn_leaf_number(v, out, NULL) == 0;
}

static GTEXT_CSS_Easing * easing_new(const GTEXT_Allocator * a) {
  GTEXT_CSS_Easing * e = gtext_allocator_calloc(a, 1, sizeof *e);
  if (e) {
    e->allocator_ = *a;
  }
  return e;
}

void gtext_css_easing_free(GTEXT_CSS_Easing * e) {
  if (!e) {
    return;
  }
  GTEXT_Allocator a = e->allocator_;
  gtext_allocator_free(&a, e->points);
  gtext_allocator_free(&a, e);
}

GTEXT_CSS_Status gtext_css_easing_from_value(const GTEXT_CSS_Value * value, const GTEXT_Allocator * allocator, bool * valid,
    GTEXT_CSS_Easing ** out) {
  if (!value || !valid || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * a = allocator ? allocator : gtext_allocator_default();
  *valid = false;
  *out = NULL;
  GTEXT_CSS_Easing * e = NULL;
  if (value->kind == GTEXT_CSS_VAL_KEYWORD) {
    static const struct { const char * n; GTEXT_CSS_Easing_Kind k; GTEXT_CSS_Easing_Keyword kw; double c[4]; int64_t steps; GTEXT_CSS_Step_Position pos; } KW[] = {
      {"linear", GTEXT_CSS_EASING_LINEAR, GTEXT_CSS_EASING_KW_NONE, {0, 0, 1, 1}, 0, GTEXT_CSS_STEP_JUMP_END},
      {"ease", GTEXT_CSS_EASING_CUBIC_BEZIER, GTEXT_CSS_EASING_KW_EASE, {0.25, 0.1, 0.25, 1.0}, 0, GTEXT_CSS_STEP_JUMP_END},
      {"ease-in", GTEXT_CSS_EASING_CUBIC_BEZIER, GTEXT_CSS_EASING_KW_EASE_IN, {0.42, 0.0, 1.0, 1.0}, 0, GTEXT_CSS_STEP_JUMP_END},
      {"ease-out", GTEXT_CSS_EASING_CUBIC_BEZIER, GTEXT_CSS_EASING_KW_EASE_OUT, {0.0, 0.0, 0.58, 1.0}, 0, GTEXT_CSS_STEP_JUMP_END},
      {"ease-in-out", GTEXT_CSS_EASING_CUBIC_BEZIER, GTEXT_CSS_EASING_KW_EASE_IN_OUT, {0.42, 0.0, 0.58, 1.0}, 0, GTEXT_CSS_STEP_JUMP_END},
      {"step-start", GTEXT_CSS_EASING_STEPS, GTEXT_CSS_EASING_KW_STEP_START, {0, 0, 0, 0}, 1, GTEXT_CSS_STEP_JUMP_START},
      {"step-end", GTEXT_CSS_EASING_STEPS, GTEXT_CSS_EASING_KW_STEP_END, {0, 0, 0, 0}, 1, GTEXT_CSS_STEP_JUMP_END},
    };
    for (size_t i = 0; i < sizeof KW / sizeof KW[0]; i++) {
      if (css_fn_name_is(&value->text, KW[i].n)) {
        e = easing_new(a);
        if (!e) {
          return GTEXT_CSS_E_OOM;
        }
        e->kind = KW[i].k;
        e->keyword = KW[i].kw;
        e->x1 = KW[i].c[0];
        e->y1 = KW[i].c[1];
        e->x2 = KW[i].c[2];
        e->y2 = KW[i].c[3];
        e->steps = KW[i].steps;
        e->position = KW[i].pos;
        /* "step-start and step-end ... serialize as steps(1, start) and steps(1)" (Easing 2, 2.3.1) */
        e->position_short = KW[i].kw == GTEXT_CSS_EASING_KW_STEP_START;
        *out = e;
        *valid = true;
        return GTEXT_CSS_OK;
      }
    }
    return GTEXT_CSS_OK; /* a keyword that is not an easing function: not valid, not an error */
  }
  if (value->kind != GTEXT_CSS_VAL_FUNCTION || value->math) {
    return GTEXT_CSS_OK;
  }
  css_leafvec lv;
  if (!css_fn_leaves(a, value, &lv)) {
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (css_fn_name_is(&value->text, "cubic-bezier")) {
    double c[4];
    size_t k = 0;
    bool good = true;
    for (size_t i = 0; i < lv.n && good; i++) {
      if (!lv.v[i]) {
        continue;
      }
      if (k >= 4 || !number_of(lv.v[i], &c[k])) {
        good = false;
      }
      k++;
    }
    /* "the x coordinates are in the range [0, 1]" (a value out of it that came from a math function is as invalid as a literal) */
    if (good && k == 4 && c[0] >= 0 && c[0] <= 1 && c[2] >= 0 && c[2] <= 1) {
      e = easing_new(a);
      if (!e) {
        st = GTEXT_CSS_E_OOM;
      }
      else {
        e->kind = GTEXT_CSS_EASING_CUBIC_BEZIER;
        e->x1 = c[0];
        e->y1 = c[1];
        e->x2 = c[2];
        e->y2 = c[3];
        *valid = true;
      }
    }
  }
  else if (css_fn_name_is(&value->text, "steps")) {
    double n = 0;
    bool good = lv.n >= 1 && lv.v[0] && css_fn_leaf_number(lv.v[0], &n, NULL) == 0;
    GTEXT_CSS_Step_Position pos = GTEXT_CSS_STEP_JUMP_END;
    bool written = false, is_short = false;
    for (size_t i = 1; good && i < lv.n; i++) {
      if (!lv.v[i]) {
        continue;
      }
      const GTEXT_CSS_String * t = &lv.v[i]->text;
      written = true;
      if (css_fn_name_is(t, "jump-start")) {
        pos = GTEXT_CSS_STEP_JUMP_START;
      }
      else if (css_fn_name_is(t, "start")) {
        pos = GTEXT_CSS_STEP_JUMP_START;
        is_short = true;
      }
      else if (css_fn_name_is(t, "jump-end")) {
        pos = GTEXT_CSS_STEP_JUMP_END;
      }
      else if (css_fn_name_is(t, "end")) {
        pos = GTEXT_CSS_STEP_JUMP_END;
        is_short = true;
      }
      else if (css_fn_name_is(t, "jump-none")) {
        pos = GTEXT_CSS_STEP_JUMP_NONE;
      }
      else if (css_fn_name_is(t, "jump-both")) {
        pos = GTEXT_CSS_STEP_JUMP_BOTH;
      }
      else {
        good = false;
      }
    }
    /* "the first parameter ... must be a positive integer, and at least 2 with jump-none" */
    /* The range is tested before the conversion: a double outside int64_t (steps(1e300), infinity) or NaN is not converted at all.
       Inside [1, 2^53] a double is an integer exactly when it converts back to itself. */
    if (good && n >= (pos == GTEXT_CSS_STEP_JUMP_NONE ? 2.0 : 1.0) && n <= 9007199254740992.0 && n == (double)(int64_t)n) {
      e = easing_new(a);
      if (!e) {
        st = GTEXT_CSS_E_OOM;
      }
      else {
        e->kind = GTEXT_CSS_EASING_STEPS;
        e->steps = (int64_t)n;
        e->position = pos;
        e->position_written = written;
        e->position_short = is_short;
        *valid = true;
      }
    }
  }
  else if (css_fn_name_is(&value->text, "linear")) {
    /* arguments: a number and up to two percentages */
    size_t args = 1;
    for (size_t i = 0; i < lv.n; i++) {
      if (!lv.v[i]) {
        args++;
      }
    }
    GTEXT_CSS_Easing_Point * pts = gtext_allocator_calloc(a, args * 2, sizeof *pts);
    if (!pts) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      size_t np = 0;
      bool good = lv.n > 0;
      double out_v = 0.0;
      size_t inputs = 0;
      double in_v[2] = {0, 0};
      bool have = false;
      for (size_t i = 0; i <= lv.n && good; i++) {
        const bool end = i == lv.n || !lv.v[i];
        if (end) {
          if (!have) {
            good = false;
            break;
          }
          if (inputs == 0) {
            pts[np].output = out_v;
            np++;
          }
          for (size_t k = 0; k < inputs; k++) {
            pts[np].output = out_v;
            pts[np].input = in_v[k] / 100.0;
            pts[np].input_written = true;
            np++;
          }
          have = false;
          inputs = 0;
          continue;
        }
        double x;
        const int kind = css_fn_leaf_number(lv.v[i], &x, NULL);
        /* `<number> && <percentage>{0,2}`: the value of a && is in the grammar's order, so the number comes first whatever order it was
           written in; exactly one number, and up to two percentages */
        if (kind == 0 && !have) {
          out_v = x;
          have = true;
        }
        else if (kind == 1 && have && inputs < 2) {
          in_v[inputs++] = x;
        }
        else {
          good = false;
        }
      }
      if (good && np > 0) {
        /* canonicalize */
        if (!pts[0].input_written) {
          pts[0].input = 0.0;
        }
        if (np > 1 && !pts[np - 1].input_written) {
          pts[np - 1].input = 1.0;
        }
        double run_max = -1.0 / 0.0;
        bool first = true;
        for (size_t i = 0; i < np; i++) {
          if (pts[i].input_written || i == 0 || i == np - 1) {
            if (!first && pts[i].input < run_max) {
              pts[i].input = run_max;
            }
            if (first || pts[i].input > run_max) {
              run_max = pts[i].input;
            }
            first = false;
          }
        }
        /* runs without an input: evenly spaced between the neighbors that have one */
        size_t i = 0;
        while (i < np) {
          if (pts[i].input_written || i == 0 || i == np - 1) {
            i++;
            continue;
          }
          size_t j = i;
          while (j < np && !(pts[j].input_written || j == np - 1)) {
            j++;
          }
          const double lo = pts[i - 1].input, hi = pts[j].input;
          const double step = (hi - lo) / (double)(j - i + 1);
          for (size_t k = i; k < j; k++) {
            pts[k].input = lo + step * (double)(k - i + 1);
          }
          i = j + 1;
        }
        e = easing_new(a);
        if (!e) {
          st = GTEXT_CSS_E_OOM;
        }
        else {
          e->kind = GTEXT_CSS_EASING_LINEAR_POINTS;
          e->points = pts;
          e->point_count = np;
          pts = NULL;
          *valid = true;
        }
      }
      gtext_allocator_free(a, pts);
    }
  }
  /* (any other function is not an easing function: *valid stays false) */
  css_fn_leaves_free(a, &lv);
  if (st != GTEXT_CSS_OK) {
    gtext_css_easing_free(e);
    return st;
  }
  if (*valid) {
    *out = e;
  }
  return GTEXT_CSS_OK;
}

/* ---- serialization ------------------------------------------------------------------- */

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
  bool oom;
} sbuf;

static bool sput(sbuf * b, const char * s, size_t n) {
  if (b->oom) {
    return false;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char * p = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!p) {
      b->oom = true;
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

static bool sputs(sbuf * b, const char * s) {
  return sput(b, s, strlen(s));
}

static bool snum(sbuf * b, double v) {
  char t[340];
  return sput(b, t, css_cssom_number(v, false, t));
}

GTEXT_CSS_Status gtext_css_easing_serialize_buffer(const GTEXT_CSS_Easing * e, const GTEXT_Allocator * allocator, char ** out,
    size_t * out_length) {
  if (!e || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  sbuf b;
  memset(&b, 0, sizeof b);
  b.alloc = *(allocator ? allocator : gtext_allocator_default());
  switch (e->kind) {
  case GTEXT_CSS_EASING_LINEAR:
    sputs(&b, "linear");
    break;
  case GTEXT_CSS_EASING_CUBIC_BEZIER:
    switch (e->keyword) {
    case GTEXT_CSS_EASING_KW_EASE: sputs(&b, "ease"); break;
    case GTEXT_CSS_EASING_KW_EASE_IN: sputs(&b, "ease-in"); break;
    case GTEXT_CSS_EASING_KW_EASE_OUT: sputs(&b, "ease-out"); break;
    case GTEXT_CSS_EASING_KW_EASE_IN_OUT: sputs(&b, "ease-in-out"); break;
    default:
      sputs(&b, "cubic-bezier(");
      snum(&b, e->x1);
      sputs(&b, ", ");
      snum(&b, e->y1);
      sputs(&b, ", ");
      snum(&b, e->x2);
      sputs(&b, ", ");
      snum(&b, e->y2);
      sputs(&b, ")");
      break;
    }
    break;
  case GTEXT_CSS_EASING_STEPS: {
    sputs(&b, "steps(");
    snum(&b, (double)e->steps);
    /* "If the step position is jump-end or end, serialize as steps(<integer>)" (Easing 1, 2.4) */
    if (e->position != GTEXT_CSS_STEP_JUMP_END) {
      sputs(&b, ", ");
      switch (e->position) {
      case GTEXT_CSS_STEP_JUMP_START: sputs(&b, e->position_short ? "start" : "jump-start"); break;
      case GTEXT_CSS_STEP_JUMP_NONE: sputs(&b, "jump-none"); break;
      default: sputs(&b, "jump-both"); break;
      }
    }
    sputs(&b, ")");
    break;
  }
  case GTEXT_CSS_EASING_LINEAR_POINTS:
    sputs(&b, "linear(");
    for (size_t i = 0; i < e->point_count; i++) {
      if (i) {
        sputs(&b, ", ");
      }
      snum(&b, e->points[i].output);
      if (e->points[i].input_written) {
        sputs(&b, " ");
        snum(&b, e->points[i].input * 100.0);
        sputs(&b, "%");
      }
    }
    sputs(&b, ")");
    break;
  }
  if (b.oom) {
    gtext_allocator_free(&b.alloc, b.data);
    return GTEXT_CSS_E_OOM;
  }
  *out = b.data;
  *out_length = b.len;
  return GTEXT_CSS_OK;
}
