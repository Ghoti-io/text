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
 * The node constructors of a calculation and the algebra under them: the type
 * of a node (Values 4, 10.9, which is the Typed OM's "add two types" and
 * "multiply two types"), the simplification the draft applies as a tree is
 * built (10.10.1), and the numeric functions of 10.3 to 10.6 with their argument
 * ranges.
 *
 * Every number is an IEEE double and every function of it is the `math`
 * library's, so the result is the same on every host (no `<math.h>` symbol
 * appears in this directory; tools/css/check-no-libm.sh refuses one).
 */

#include <ghoti.io/math/basic.h>
#include <ghoti.io/math/elementary.h>

#include <float.h>
#include <stdint.h>
#include <string.h>

#include "css_math_internal.h"

#define B_PERCENT GTEXT_CSS_MATH_BASE_PERCENT
#define NBASE GTEXT_CSS_MATH_BASE_COUNT

#define PI_VALUE 3.14159265358979323846264338327950288

/* ---- IEEE helpers (no <math.h>) ----------------------------------------- */

static bool is_nan(double x) {
  return x != x;
}

static bool is_inf(double x) {
  return x > DBL_MAX || x < -DBL_MAX;
}

static bool is_neg(double x) {
  return gmath_copysign(1.0, x) < 0.0;
}

static double make_inf(void) {
  volatile double z = 0.0;
  return 1.0 / z;
}

static double make_nan(void) {
  volatile double z = 0.0;
  return z / z;
}

/* ---- builder ------------------------------------------------------------ */

void mb_fail(mbuilder * mb, GTEXT_CSS_Status st) {
  if (mb->status == GTEXT_CSS_OK) {
    mb->status = st;
  }
}

void mb_invalid(mbuilder * mb) {
  mb->invalid = true;
}

mnode * mb_node(mbuilder * mb, GTEXT_CSS_Math_Op op) {
  if (mb->status != GTEXT_CSS_OK) {
    return NULL;
  }
  if (mb->nodes >= mb->max_nodes) {
    mb_fail(mb, GTEXT_CSS_E_LIMIT_MATH_NODES);
    return NULL;
  }
  mnode * n = css_arena_alloc(mb->arena, sizeof *n);
  if (!n) {
    mb_fail(mb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  memset(n, 0, sizeof *n);
  n->op = op;
  mb->nodes++;
  mb->work++;
  return n;
}

const mnode ** mb_kids(mbuilder * mb, size_t n) {
  if (n == 0) {
    return NULL;
  }
  if (n > SIZE_MAX / sizeof(mnode *)) {
    mb_fail(mb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  const mnode ** k = css_arena_alloc(mb->arena, n * sizeof *k);
  if (!k) {
    mb_fail(mb, GTEXT_CSS_E_OOM);
  }
  return k;
}

/* ---- types --------------------------------------------------------------- */

static bool exps_equal(const GTEXT_CSS_Math_Type * a, const GTEXT_CSS_Math_Type * b) {
  for (int i = 0; i < NBASE; i++) {
    if (a->exponent[i] != b->exponent[i]) {
      return false;
    }
  }
  return true;
}

/// "Apply the percent hint" of the Typed OM to `t`: the percent exponent moves to the hint's base type.
static bool apply_hint(GTEXT_CSS_Math_Type * t, int hint) {
  if (hint != B_PERCENT) {
    const int s = (int)t->exponent[hint] + (int)t->exponent[B_PERCENT];
    if (s > 127 || s < -127) {
      return false;
    }
    t->exponent[hint] = (int8_t)s;
    t->exponent[B_PERCENT] = 0;
  }
  t->percent_hint = (uint8_t)(hint + 1);
  return true;
}

/// Step 1 and 2 of both operations: make the percent hints agree, or fail.
static bool unify_hints(GTEXT_CSS_Math_Type * t1, GTEXT_CSS_Math_Type * t2) {
  if (t1->percent_hint == t2->percent_hint) {
    return true;
  }
  if (t1->percent_hint && t2->percent_hint) {
    return false;
  }
  if (!t1->percent_hint) {
    return apply_hint(t1, t2->percent_hint - 1);
  }
  return apply_hint(t2, t1->percent_hint - 1);
}

bool css_math_type_add(const GTEXT_CSS_Math_Type * a, const GTEXT_CSS_Math_Type * b, GTEXT_CSS_Math_Type * out) {
  GTEXT_CSS_Math_Type t1 = *a, t2 = *b;
  if (!unify_hints(&t1, &t2)) {
    return false;
  }
  if (exps_equal(&t1, &t2)) {
    *out = t1;
    return true;
  }
  bool pct = t1.exponent[B_PERCENT] != 0 || t2.exponent[B_PERCENT] != 0;
  bool other = false;
  for (int i = 0; i < B_PERCENT; i++) {
    if (t1.exponent[i] != 0 || t2.exponent[i] != 0) {
      other = true;
    }
  }
  if (pct && other) {
    for (int k = 0; k < B_PERCENT; k++) {
      GTEXT_CSS_Math_Type p1 = t1, p2 = t2;
      if (!apply_hint(&p1, k) || !apply_hint(&p2, k)) {
        continue;
      }
      if (exps_equal(&p1, &p2)) {
        *out = p1;
        return true;
      }
    }
  }
  return false;
}

bool css_math_type_mul(const GTEXT_CSS_Math_Type * a, const GTEXT_CSS_Math_Type * b, GTEXT_CSS_Math_Type * out) {
  GTEXT_CSS_Math_Type t1 = *a, t2 = *b;
  if (!unify_hints(&t1, &t2)) {
    return false;
  }
  GTEXT_CSS_Math_Type r = t1;
  for (int i = 0; i < NBASE; i++) {
    const int s = (int)t1.exponent[i] + (int)t2.exponent[i];
    if (s > 127 || s < -127) {
      return false;
    }
    r.exponent[i] = (int8_t)s;
  }
  *out = r;
  return true;
}

GTEXT_CSS_Math_Type css_math_type_invert(const GTEXT_CSS_Math_Type * a) {
  GTEXT_CSS_Math_Type r = *a;
  for (int i = 0; i < NBASE; i++) {
    r.exponent[i] = (int8_t)-a->exponent[i];
  }
  return r;
}

GTEXT_CSS_Math_Category css_math_type_category(const GTEXT_CSS_Math_Type * t) {
  int nz = 0, idx = -1;
  for (int i = 0; i < NBASE; i++) {
    if (t->exponent[i] != 0) {
      nz++;
      idx = i;
    }
  }
  if (nz == 0) {
    return GTEXT_CSS_MATH_CAT_NUMBER;
  }
  if (nz != 1 || t->exponent[idx] != 1) {
    return GTEXT_CSS_MATH_CAT_NONE;
  }
  switch (idx) {
  case GTEXT_CSS_MATH_BASE_LENGTH: return GTEXT_CSS_MATH_CAT_LENGTH;
  case GTEXT_CSS_MATH_BASE_ANGLE: return GTEXT_CSS_MATH_CAT_ANGLE;
  case GTEXT_CSS_MATH_BASE_TIME: return GTEXT_CSS_MATH_CAT_TIME;
  case GTEXT_CSS_MATH_BASE_FREQUENCY: return GTEXT_CSS_MATH_CAT_FREQUENCY;
  case GTEXT_CSS_MATH_BASE_RESOLUTION: return GTEXT_CSS_MATH_CAT_RESOLUTION;
  case GTEXT_CSS_MATH_BASE_FLEX: return GTEXT_CSS_MATH_CAT_FLEX;
  default: return GTEXT_CSS_MATH_CAT_PERCENTAGE;
  }
}

bool css_math_type_is_number(const GTEXT_CSS_Math_Type * t) {
  return css_math_type_category(t) == GTEXT_CSS_MATH_CAT_NUMBER;
}

GTEXT_CSS_Math_Type css_math_unit_type(GTEXT_CSS_Unit unit) {
  GTEXT_CSS_Math_Type t;
  memset(&t, 0, sizeof t);
  switch (gtext_css_unit_type(unit)) {
  case GTEXT_CSS_UNIT_TYPE_LENGTH: t.exponent[GTEXT_CSS_MATH_BASE_LENGTH] = 1; break;
  case GTEXT_CSS_UNIT_TYPE_ANGLE: t.exponent[GTEXT_CSS_MATH_BASE_ANGLE] = 1; break;
  case GTEXT_CSS_UNIT_TYPE_TIME: t.exponent[GTEXT_CSS_MATH_BASE_TIME] = 1; break;
  case GTEXT_CSS_UNIT_TYPE_FREQUENCY: t.exponent[GTEXT_CSS_MATH_BASE_FREQUENCY] = 1; break;
  case GTEXT_CSS_UNIT_TYPE_RESOLUTION: t.exponent[GTEXT_CSS_MATH_BASE_RESOLUTION] = 1; break;
  case GTEXT_CSS_UNIT_TYPE_FLEX: t.exponent[GTEXT_CSS_MATH_BASE_FLEX] = 1; break;
  default: break;
  }
  return t;
}

GTEXT_CSS_Unit css_math_category_unit(GTEXT_CSS_Math_Category cat) {
  switch (cat) {
  case GTEXT_CSS_MATH_CAT_LENGTH: return GTEXT_CSS_UNIT_PX;
  case GTEXT_CSS_MATH_CAT_ANGLE: return GTEXT_CSS_UNIT_DEG;
  case GTEXT_CSS_MATH_CAT_TIME: return GTEXT_CSS_UNIT_S;
  case GTEXT_CSS_MATH_CAT_FREQUENCY: return GTEXT_CSS_UNIT_HZ;
  case GTEXT_CSS_MATH_CAT_RESOLUTION: return GTEXT_CSS_UNIT_DPPX;
  case GTEXT_CSS_MATH_CAT_FLEX: return GTEXT_CSS_UNIT_FR;
  default: return GTEXT_CSS_UNIT_NONE;
  }
}

GTEXT_CSS_Math_Type gtext_css_math_category_type(GTEXT_CSS_Math_Category category) {
  GTEXT_CSS_Math_Type t;
  memset(&t, 0, sizeof t);
  switch (category) {
  case GTEXT_CSS_MATH_CAT_PERCENTAGE: t.exponent[B_PERCENT] = 1; break;
  case GTEXT_CSS_MATH_CAT_LENGTH: t.exponent[GTEXT_CSS_MATH_BASE_LENGTH] = 1; break;
  case GTEXT_CSS_MATH_CAT_ANGLE: t.exponent[GTEXT_CSS_MATH_BASE_ANGLE] = 1; break;
  case GTEXT_CSS_MATH_CAT_TIME: t.exponent[GTEXT_CSS_MATH_BASE_TIME] = 1; break;
  case GTEXT_CSS_MATH_CAT_FREQUENCY: t.exponent[GTEXT_CSS_MATH_BASE_FREQUENCY] = 1; break;
  case GTEXT_CSS_MATH_CAT_RESOLUTION: t.exponent[GTEXT_CSS_MATH_BASE_RESOLUTION] = 1; break;
  case GTEXT_CSS_MATH_CAT_FLEX: t.exponent[GTEXT_CSS_MATH_BASE_FLEX] = 1; break;
  default: break;
  }
  return t;
}

/* ---- the numeric functions --------------------------------------------- */

/// min of two, with NaN infectious and -0 below +0.
static double min2(double a, double b) {
  if (is_nan(a) || is_nan(b)) {
    return make_nan();
  }
  if (a == b) {
    return is_neg(a) ? a : b;
  }
  return a < b ? a : b;
}

static double max2(double a, double b) {
  if (is_nan(a) || is_nan(b)) {
    return make_nan();
  }
  if (a == b) {
    return is_neg(a) ? b : a;
  }
  return a > b ? a : b;
}

double css_math_fmod_exact(double x, double y) {
  if (is_nan(x) || is_nan(y) || is_inf(x) || y == 0.0) {
    return make_nan();
  }
  if (is_inf(y)) {
    return x;
  }
  double ax = gmath_fabs(x);
  const double ay = gmath_fabs(y);
  if (ax < ay) {
    return x;
  }
  int ex = 0, ey = 0;
  (void)gmath_frexp(ax, &ex);
  (void)gmath_frexp(ay, &ey);
  /* ax < 2^ex and ay >= 2^(ey-1), so ay * 2^(ex-ey+1) is above ax: the loop starts with a shift that cannot fit */
  for (int k = ex - ey + 1; k >= 0; k--) {
    const double t = gmath_scalbn(ay, k);
    if (ax >= t) {
      ax -= t; /* t <= ax < 2t, so the difference is exact */
    }
  }
  return gmath_copysign(ax, x);
}

static double round_value(GTEXT_CSS_Math_Round strategy, double A, double B) {
  if (is_nan(A) || is_nan(B) || B == 0.0) {
    return make_nan();
  }
  if (is_inf(A)) {
    return is_inf(B) ? make_nan() : A;
  }
  if (is_inf(B)) {
    switch (strategy) {
    case GTEXT_CSS_MATH_ROUND_UP:
      if (A > 0.0) {
        return make_inf();
      }
      return (A == 0.0 && !is_neg(A)) ? 0.0 : -0.0;
    case GTEXT_CSS_MATH_ROUND_DOWN:
      if (A < 0.0) {
        return -make_inf();
      }
      return (A == 0.0 && is_neg(A)) ? -0.0 : 0.0;
    default:
      return (A > 0.0 || (A == 0.0 && !is_neg(A))) ? 0.0 : -0.0;
    }
  }
  const double b = gmath_fabs(B);
  const double q = A / b;
  const double lq = gmath_floor(q), uq = gmath_ceil(q);
  if (lq == uq) {
    return A;
  }
  double lower = lq * b, upper = uq * b;
  if (lq == 0.0) {
    lower = 0.0;
  }
  if (uq == 0.0) {
    upper = -0.0;
  }
  switch (strategy) {
  case GTEXT_CSS_MATH_ROUND_UP: return upper;
  case GTEXT_CSS_MATH_ROUND_DOWN: return lower;
  case GTEXT_CSS_MATH_ROUND_TO_ZERO: return A > 0.0 ? lower : upper;
  default: {
    const double dl = A - lower, du = upper - A;
    return dl < du ? lower : upper;
  }
  }
}

/// Round to a double: on a target that evaluates in a wider format (x87) an intermediate of an expression would otherwise keep the extra
/// bits and the result would differ from the one of a host that rounds each operation. The way css_units.c does it.
static inline double rnd(double x) {
  volatile double v = x;
  return v;
}

/// The double next to |x| toward zero, with the sign of x (x finite and not zero).
static double toward_zero(double x) {
  uint64_t bits;
  memcpy(&bits, &x, sizeof bits);
  bits--;
  memcpy(&x, &bits, sizeof x);
  return x;
}

static double mod_value(double A, double B) {
  if (is_nan(A) || is_nan(B) || B == 0.0 || is_inf(A)) {
    return make_nan();
  }
  if (is_inf(B)) {
    return is_neg(A) != is_neg(B) ? make_nan() : A;
  }
  const double r = css_math_fmod_exact(A, B);
  if (r == 0.0) {
    return gmath_copysign(0.0, B);
  }
  if ((r < 0.0) != (B < 0.0)) {
    /* r + B rounds to B itself when r is far below B's last bit (mod(-1e-20, 1)): the result is below |B|, so it is the double next to B */
    const double s = rnd(r + B);
    return gmath_fabs(s) >= gmath_fabs(B) ? toward_zero(B) : s;
  }
  return r;
}

static double rem_value(double A, double B) {
  if (is_nan(A) || is_nan(B) || B == 0.0 || is_inf(A)) {
    return make_nan();
  }
  if (is_inf(B)) {
    return A;
  }
  return css_math_fmod_exact(A, B);
}

static double pow_value(double A, double B) {
  if (is_nan(A) || is_nan(B)) {
    return make_nan();
  }
  if (is_inf(B) && (A == 1.0 || A == -1.0)) {
    return make_nan();
  }
  return gmath_pow(A, B);
}

static double hypot_value(const double * a, size_t n) {
  double m = 0.0;
  bool inf = false;
  for (size_t i = 0; i < n; i++) {
    if (is_nan(a[i])) {
      return make_nan();
    }
    if (is_inf(a[i])) {
      inf = true;
    }
  }
  if (inf) {
    return make_inf();
  }
  for (size_t i = 0; i < n; i++) {
    const double x = gmath_fabs(a[i]);
    if (x > m) {
      m = x;
    }
  }
  if (m == 0.0 || n == 1) {
    return m;
  }
  if (n == 2) {
    return gmath_hypot(a[0], a[1]);
  }
  double sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    const double r = rnd(a[i] / m);
    sum = rnd(sum + rnd(r * r));
  }
  return rnd(m * rnd(gmath_sqrt(sum)));
}

static double log_value(double A, bool has_base, double B) {
  if (is_nan(A) || (has_base && is_nan(B))) {
    return make_nan();
  }
  if (A < 0.0) {
    return make_nan();
  }
  if (has_base && (B == 1.0 || B < 0.0)) {
    return make_nan();
  }
  if (A == 0.0) {
    return -make_inf();
  }
  if (A == 1.0) {
    return 0.0;
  }
  if (!has_base) {
    return gmath_log(A);
  }
  const double la = rnd(gmath_log(A)), lb = rnd(gmath_log(B));
  return rnd(la / lb);
}

bool css_math_apply(GTEXT_CSS_Math_Op op, GTEXT_CSS_Math_Round round, const double * a, size_t n, double * out) {
  return css_math_apply_ex(op, round, false, a, n, out);
}

bool css_math_apply_ex(GTEXT_CSS_Math_Op op, GTEXT_CSS_Math_Round round, bool no_clamp, const double * a, size_t n, double * out) {
  switch (op) {
  case GTEXT_CSS_MATH_MIN: case GTEXT_CSS_MATH_MAX: {
    if (n == 0) {
      return false;
    }
    double r = a[0];
    for (size_t i = 1; i < n; i++) {
      r = op == GTEXT_CSS_MATH_MIN ? min2(r, a[i]) : max2(r, a[i]);
    }
    if (is_nan(r)) {
      r = make_nan();
    }
    *out = r;
    return true;
  }
  case GTEXT_CSS_MATH_CLAMP:
    if (n != 3) {
      return false;
    }
    *out = max2(a[0], min2(a[1], a[2]));
    return true;
  case GTEXT_CSS_MATH_ROUND:
    if (n == 1) {
      *out = round_value(round, a[0], 1.0);
      return true;
    }
    if (n != 2) {
      return false;
    }
    *out = round_value(round, a[0], a[1]);
    return true;
  case GTEXT_CSS_MATH_MOD:
    if (n != 2) {
      return false;
    }
    *out = mod_value(a[0], a[1]);
    return true;
  case GTEXT_CSS_MATH_REM:
    if (n != 2) {
      return false;
    }
    *out = rem_value(a[0], a[1]);
    return true;
  case GTEXT_CSS_MATH_SIN: case GTEXT_CSS_MATH_COS: case GTEXT_CSS_MATH_TAN: {
    if (n != 1) {
      return false;
    }
    if (is_nan(a[0]) || is_inf(a[0])) {
      *out = make_nan();
      return true;
    }
    *out = op == GTEXT_CSS_MATH_SIN ? gmath_sin(a[0]) : op == GTEXT_CSS_MATH_COS ? gmath_cos(a[0]) : gmath_tan(a[0]);
    return true;
  }
  case GTEXT_CSS_MATH_ASIN: case GTEXT_CSS_MATH_ACOS: {
    if (n != 1) {
      return false;
    }
    const double x = a[0];
    if (is_nan(x) || x > 1.0 || x < -1.0) {
      *out = make_nan();
      return true;
    }
    if (op == GTEXT_CSS_MATH_ASIN && x == 0.0) {
      *out = x;
      return true;
    }
    const double s = rnd(gmath_sqrt(rnd(rnd(1.0 - x) * rnd(1.0 + x))));
    *out = op == GTEXT_CSS_MATH_ASIN ? gmath_atan2(x, s) : gmath_atan2(s, x);
    return true;
  }
  case GTEXT_CSS_MATH_ATAN:
    if (n != 1) {
      return false;
    }
    *out = is_nan(a[0]) ? make_nan() : gmath_atan2(a[0], 1.0);
    return true;
  case GTEXT_CSS_MATH_ATAN2:
    if (n != 2) {
      return false;
    }
    *out = (is_nan(a[0]) || is_nan(a[1])) ? make_nan() : gmath_atan2(a[0], a[1]);
    return true;
  case GTEXT_CSS_MATH_POW:
    if (n != 2) {
      return false;
    }
    *out = pow_value(a[0], a[1]);
    return true;
  case GTEXT_CSS_MATH_SQRT:
    if (n != 1) {
      return false;
    }
    *out = (is_nan(a[0]) || a[0] < 0.0) ? make_nan() : gmath_sqrt(a[0]);
    return true;
  case GTEXT_CSS_MATH_HYPOT:
    if (n == 0) {
      return false;
    }
    *out = hypot_value(a, n);
    return true;
  case GTEXT_CSS_MATH_LOG:
    if (n != 1 && n != 2) {
      return false;
    }
    *out = log_value(a[0], n == 2, n == 2 ? a[1] : 0.0);
    return true;
  case GTEXT_CSS_MATH_EXP:
    if (n != 1) {
      return false;
    }
    *out = is_nan(a[0]) ? make_nan() : gmath_exp(a[0]);
    return true;
  case GTEXT_CSS_MATH_ABS:
    if (n != 1) {
      return false;
    }
    *out = is_nan(a[0]) ? make_nan() : gmath_fabs(a[0]);
    return true;
  case GTEXT_CSS_MATH_SIGN:
    if (n != 1) {
      return false;
    }
    *out = is_nan(a[0]) ? make_nan() : a[0] > 0.0 ? 1.0 : a[0] < 0.0 ? -1.0 : a[0];
    return true;
  case GTEXT_CSS_MATH_PROGRESS: {
    if (n != 3) {
      return false;
    }
    const double v = a[0], from = a[1], to = a[2];
    if (is_nan(v) || is_nan(from) || is_nan(to)) {
      *out = make_nan();
      return true;
    }
    double r;
    if (from != to) {
      r = (v - from) / (to - from);
      if (!no_clamp) {
        r = r < 0.0 ? 0.0 : r > 1.0 ? 1.0 : r;
      }
    }
    else if (!no_clamp) {
      r = 0.0;
    }
    else {
      r = v == from ? 0.0 : v < from ? -make_inf() : make_inf();
    }
    *out = r;
    return true;
  }
  default:
    return false;
  }
}

/* ---- leaves -------------------------------------------------------------- */

bool css_math_is_numeric(const mnode * n) {
  return n->op == GTEXT_CSS_MATH_NUMBER || n->op == GTEXT_CSS_MATH_DIMENSION || n->op == GTEXT_CSS_MATH_PERCENT;
}

/// A number, or a dimension in an absolute (hence canonical) unit: a leaf whose value an operation can use.
static bool foldable(const mnode * n) {
  return n->op == GTEXT_CSS_MATH_NUMBER || (n->op == GTEXT_CSS_MATH_DIMENSION && gtext_css_unit_is_absolute(n->unit));
}

const mnode * mk_number(mbuilder * mb, double v) {
  mnode * n = mb_node(mb, GTEXT_CSS_MATH_NUMBER);
  if (n) {
    n->value = v;
  }
  return n;
}

const mnode * mk_dimension(mbuilder * mb, double v, GTEXT_CSS_Unit unit) {
  double c;
  if (gtext_css_unit_is_absolute(unit) && gtext_css_unit_to_canonical(unit, v, &c)) {
    v = c;
    unit = gtext_css_unit_canonical(unit);
  }
  mnode * n = mb_node(mb, GTEXT_CSS_MATH_DIMENSION);
  if (n) {
    n->value = v;
    n->unit = unit;
    n->type = css_math_unit_type(unit);
  }
  return n;
}

const mnode * mk_percent(mbuilder * mb, double v) {
  mnode * n = mb_node(mb, GTEXT_CSS_MATH_PERCENT);
  if (n) {
    n->value = v;
    n->type.exponent[B_PERCENT] = 1;
  }
  return n;
}

/// A leaf of the kind and unit of `like` with value `v`.
static const mnode * leaf_like(mbuilder * mb, const mnode * like, double v) {
  switch (like->op) {
  case GTEXT_CSS_MATH_NUMBER: return mk_number(mb, v);
  case GTEXT_CSS_MATH_PERCENT: return mk_percent(mb, v);
  default: return mk_dimension(mb, v, like->unit);
  }
}

static bool copy_text(mbuilder * mb, const char * s, size_t len, GTEXT_CSS_String * out) {
  char * p = css_arena_strdup(mb->arena, s, len);
  if (!p) {
    mb_fail(mb, GTEXT_CSS_E_OOM);
    return false;
  }
  out->data = p;
  out->length = len;
  return true;
}

const mnode * mk_symbol(mbuilder * mb, const char * name, size_t len, const GTEXT_CSS_Math_Type * type) {
  mnode * n = mb_node(mb, GTEXT_CSS_MATH_SYMBOL);
  if (n) {
    n->type = *type;
    if (!copy_text(mb, name, len, &n->text)) {
      return NULL;
    }
  }
  return n;
}

const mnode * mk_unresolved(mbuilder * mb, const char * text, size_t len, const GTEXT_CSS_Math_Type * type) {
  mnode * n = mb_node(mb, GTEXT_CSS_MATH_UNRESOLVED);
  if (n) {
    n->type = *type;
    if (!copy_text(mb, text, len, &n->text)) {
      return NULL;
    }
  }
  return n;
}

/* ---- negate and invert --------------------------------------------------- */

const mnode * mk_negate(mbuilder * mb, const mnode * x) {
  if (!x) {
    return NULL;
  }
  mb->work++;
  if (css_math_is_numeric(x)) {
    return leaf_like(mb, x, -x->value);
  }
  if (x->op == GTEXT_CSS_MATH_NEGATE) {
    return x->kids[0];
  }
  if (x->op == GTEXT_CSS_MATH_SUM) {
    const mnode ** kids = mb_kids(mb, x->kid_count);
    if (!kids) {
      return NULL;
    }
    for (size_t i = 0; i < x->kid_count; i++) {
      const mnode * g = x->kids[i];
      mb->work++;
      if (css_math_is_numeric(g)) {
        kids[i] = leaf_like(mb, g, -g->value);
      }
      else if (g->op == GTEXT_CSS_MATH_NEGATE) {
        kids[i] = g->kids[0];
      }
      else {
        mnode * ng = mb_node(mb, GTEXT_CSS_MATH_NEGATE);
        if (ng) {
          const mnode ** k1 = mb_kids(mb, 1);
          if (k1) {
            k1[0] = g;
            ng->kids = k1;
            ng->kid_count = 1;
            ng->type = g->type;
          }
        }
        kids[i] = ng;
      }
      if (!kids[i]) {
        return NULL;
      }
    }
    mnode * s = mb_node(mb, GTEXT_CSS_MATH_SUM);
    if (!s) {
      return NULL;
    }
    s->kids = kids;
    s->kid_count = x->kid_count;
    s->type = x->type;
    return s;
  }
  mnode * n = mb_node(mb, GTEXT_CSS_MATH_NEGATE);
  const mnode ** k1 = n ? mb_kids(mb, 1) : NULL;
  if (!n || !k1) {
    return NULL;
  }
  k1[0] = x;
  n->kids = k1;
  n->kid_count = 1;
  n->type = x->type;
  return n;
}

const mnode * mk_invert(mbuilder * mb, const mnode * x) {
  if (!x) {
    return NULL;
  }
  mb->work++;
  if (x->op == GTEXT_CSS_MATH_NUMBER) {
    return mk_number(mb, 1.0 / x->value);
  }
  if (x->op == GTEXT_CSS_MATH_INVERT) {
    return x->kids[0];
  }
  mnode * n = mb_node(mb, GTEXT_CSS_MATH_INVERT);
  const mnode ** k1 = n ? mb_kids(mb, 1) : NULL;
  if (!n || !k1) {
    return NULL;
  }
  k1[0] = x;
  n->kids = k1;
  n->kid_count = 1;
  n->type = css_math_type_invert(&x->type);
  return n;
}

/* ---- sum ------------------------------------------------------------------ */

/// Which group of a sum a numeric leaf is in: numbers, percentages, and each unit.
static size_t group_key(const mnode * n) {
  if (n->op == GTEXT_CSS_MATH_NUMBER) {
    return 0;
  }
  if (n->op == GTEXT_CSS_MATH_PERCENT) {
    return 1;
  }
  return 2 + (size_t)n->unit;
}

#define GROUPS (2 + (size_t)GTEXT_CSS_UNIT_COUNT)

const mnode * mk_sum(mbuilder * mb, const mnode * const * terms, size_t n) {
  if (n == 0) {
    mb_invalid(mb);
    return NULL;
  }
  if (n == 1) {
    return terms[0];
  }
  /* flatten (the nested sums are flat already) */
  size_t total = 0;
  for (size_t i = 0; i < n; i++) {
    total += terms[i]->op == GTEXT_CSS_MATH_SUM ? terms[i]->kid_count : 1;
  }
  const GTEXT_Allocator * a = &mb->alloc;
  const mnode ** flat = gtext_allocator_malloc(a, total * sizeof *flat);
  double * vals = gtext_allocator_malloc(a, total * sizeof *vals);
  int32_t * slot = gtext_allocator_malloc(a, GROUPS * sizeof *slot);
  unsigned char * mf = gtext_allocator_calloc(a, total, 1);
  if (!flat || !vals || !slot || !mf) {
    gtext_allocator_free(a, flat);
    gtext_allocator_free(a, vals);
    gtext_allocator_free(a, slot);
    gtext_allocator_free(a, mf);
    mb_fail(mb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  for (size_t i = 0; i < GROUPS; i++) {
    slot[i] = -1;
  }
  size_t m = 0;
  bool merged = false;
  const mnode * result = NULL;
  for (size_t i = 0; i < n; i++) {
    const mnode * t = terms[i];
    const size_t cnt = t->op == GTEXT_CSS_MATH_SUM ? t->kid_count : 1;
    for (size_t j = 0; j < cnt; j++) {
      const mnode * c = t->op == GTEXT_CSS_MATH_SUM ? t->kids[j] : t;
      mb->work++;
      if (css_math_is_numeric(c)) {
        const size_t key = group_key(c);
        if (slot[key] >= 0) {
          vals[slot[key]] += c->value;
          mf[slot[key]] = 1;
          merged = true;
          continue;
        }
        slot[key] = (int32_t)m;
        vals[m] = c->value;
      }
      flat[m++] = c;
    }
  }
  /* new leaves for the groups that were combined */
  if (merged) {
    for (size_t i = 0; i < m; i++) {
      if (mf[i]) {
        flat[i] = leaf_like(mb, flat[i], vals[i]);
        if (!flat[i]) {
          goto done;
        }
      }
    }
  }
  if (m == 1) {
    result = flat[0];
    goto done;
  }
  {
    GTEXT_CSS_Math_Type t = flat[0]->type;
    for (size_t i = 1; i < m; i++) {
      if (!css_math_type_add(&t, &flat[i]->type, &t)) {
        mb_invalid(mb);
        goto done;
      }
    }
    mnode * s = mb_node(mb, GTEXT_CSS_MATH_SUM);
    const mnode ** kids = s ? mb_kids(mb, m) : NULL;
    if (!s || !kids) {
      goto done;
    }
    memcpy(kids, flat, m * sizeof *kids);
    s->kids = kids;
    s->kid_count = m;
    s->type = t;
    result = s;
  }
done:
  gtext_allocator_free(a, flat);
  gtext_allocator_free(a, vals);
  gtext_allocator_free(a, slot);
  gtext_allocator_free(a, mf);
  return result;
}

/* ---- product -------------------------------------------------------------- */

static bool inverted_numeric(const mnode * n) {
  return n->op == GTEXT_CSS_MATH_INVERT && css_math_is_numeric(n->kids[0]) && n->kids[0]->op != GTEXT_CSS_MATH_NUMBER;
}

/// The leaf for a value of a category (a number, a percentage, or a dimension in the canonical unit).
static const mnode * leaf_of_category(mbuilder * mb, GTEXT_CSS_Math_Category cat, double v) {
  switch (cat) {
  case GTEXT_CSS_MATH_CAT_NUMBER: return mk_number(mb, v);
  case GTEXT_CSS_MATH_CAT_PERCENTAGE: return mk_percent(mb, v);
  default: return mk_dimension(mb, v, css_math_category_unit(cat));
  }
}

const mnode * mk_product(mbuilder * mb, const mnode * const * factors, size_t n) {
  if (n == 0) {
    mb_invalid(mb);
    return NULL;
  }
  if (n == 1) {
    return factors[0];
  }
  size_t total = 0;
  for (size_t i = 0; i < n; i++) {
    total += factors[i]->op == GTEXT_CSS_MATH_PRODUCT ? factors[i]->kid_count : 1;
  }
  const GTEXT_Allocator * a = &mb->alloc;
  const mnode ** flat = gtext_allocator_malloc(a, total * sizeof *flat);
  if (!flat) {
    mb_fail(mb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  const mnode * result = NULL;
  size_t m = 0, nums = 0;
  double numprod = 1.0;
  size_t first_num = 0;
  for (size_t i = 0; i < n; i++) {
    const mnode * f = factors[i];
    const size_t cnt = f->op == GTEXT_CSS_MATH_PRODUCT ? f->kid_count : 1;
    for (size_t j = 0; j < cnt; j++) {
      const mnode * c = f->op == GTEXT_CSS_MATH_PRODUCT ? f->kids[j] : f;
      mb->work++;
      if (c->op == GTEXT_CSS_MATH_NUMBER) {
        if (nums == 0) {
          first_num = m;
          numprod = c->value;
        }
        else {
          numprod *= c->value;
        }
        nums++;
        if (nums > 1) {
          continue;
        }
      }
      flat[m++] = c;
    }
  }
  if (nums > 1) {
    flat[first_num] = mk_number(mb, numprod);
    if (!flat[first_num]) {
      goto done;
    }
  }
  if (m == 1) {
    result = flat[0];
    goto done;
  }
  /* a number and a sum of numeric values: distribute */
  if (m == 2) {
    const mnode * num = flat[0]->op == GTEXT_CSS_MATH_NUMBER ? flat[0] : flat[1]->op == GTEXT_CSS_MATH_NUMBER ? flat[1] : NULL;
    const mnode * sum = num == flat[0] ? flat[1] : flat[0];
    if (num && sum->op == GTEXT_CSS_MATH_SUM) {
      bool all = true;
      for (size_t i = 0; i < sum->kid_count; i++) {
        if (!css_math_is_numeric(sum->kids[i])) {
          all = false;
          break;
        }
      }
      if (all) {
        const mnode ** kids = mb_kids(mb, sum->kid_count);
        if (!kids) {
          goto done;
        }
        for (size_t i = 0; i < sum->kid_count; i++) {
          kids[i] = leaf_like(mb, sum->kids[i], num->value * sum->kids[i]->value);
          if (!kids[i]) {
            goto done;
          }
        }
        mnode * s = mb_node(mb, GTEXT_CSS_MATH_SUM);
        if (!s) {
          goto done;
        }
        s->kids = kids;
        s->kid_count = sum->kid_count;
        s->type = sum->type;
        result = s;
        goto done;
      }
    }
  }
  {
    GTEXT_CSS_Math_Type t = flat[0]->type;
    for (size_t i = 1; i < m; i++) {
      if (!css_math_type_mul(&t, &flat[i]->type, &t)) {
        mb_invalid(mb);
        goto done;
      }
    }
    /* only numeric values and inverted numeric values: compute, if the product is a type a property can take */
    bool numeric = true;
    size_t dims = 0;
    bool any_inverted = false, all_canonical = true;
    for (size_t i = 0; i < m; i++) {
      const mnode * c = flat[i];
      if (c->op == GTEXT_CSS_MATH_NUMBER) {
        continue;
      }
      if (css_math_is_numeric(c) || inverted_numeric(c)) {
        const mnode * leaf = c->op == GTEXT_CSS_MATH_INVERT ? c->kids[0] : c;
        dims++;
        if (c->op == GTEXT_CSS_MATH_INVERT) {
          any_inverted = true;
        }
        if (!(leaf->op == GTEXT_CSS_MATH_PERCENT || foldable(leaf))) {
          all_canonical = false;
        }
        continue;
      }
      numeric = false;
      break;
    }
    const GTEXT_CSS_Math_Category cat = css_math_type_category(&t);
    if (numeric && cat != GTEXT_CSS_MATH_CAT_NONE) {
      if (dims == 1 && !any_inverted) {
        double acc = 1.0;
        const mnode * like = NULL;
        for (size_t i = 0; i < m; i++) {
          acc *= flat[i]->value;
          if (flat[i]->op != GTEXT_CSS_MATH_NUMBER) {
            like = flat[i];
          }
        }
        /* the multiplication above started from 1, which changes nothing in IEEE arithmetic */
        result = leaf_like(mb, like, acc);
        goto done;
      }
      if (all_canonical) {
        double acc = 1.0;
        for (size_t i = 0; i < m; i++) {
          if (flat[i]->op == GTEXT_CSS_MATH_INVERT) {
            acc /= flat[i]->kids[0]->value;
          }
          else {
            acc *= flat[i]->value;
          }
        }
        result = leaf_of_category(mb, cat, acc);
        goto done;
      }
    }
    mnode * p = mb_node(mb, GTEXT_CSS_MATH_PRODUCT);
    const mnode ** kids = p ? mb_kids(mb, m) : NULL;
    if (!p || !kids) {
      goto done;
    }
    memcpy(kids, flat, m * sizeof *kids);
    p->kids = kids;
    p->kid_count = m;
    p->type = t;
    result = p;
  }
done:
  gtext_allocator_free(a, flat);
  return result;
}

/* ---- functions ------------------------------------------------------------ */

static const mnode * fn_node(mbuilder * mb, GTEXT_CSS_Math_Op op, const mnode * const * args, size_t n,
    const GTEXT_CSS_Math_Type * type, const mfn_opts * opts) {
  mnode * f = mb_node(mb, op);
  const mnode ** kids = f ? mb_kids(mb, n) : NULL;
  if (!f || (n && !kids)) {
    return NULL;
  }
  if (n) {
    memcpy(kids, args, n * sizeof *kids);
  }
  f->kids = kids;
  f->kid_count = n;
  f->type = *type;
  f->round = opts->round;
  f->strategy_written = opts->strategy_written;
  f->no_clamp = opts->no_clamp;
  if (opts->key) {
    f->text.data = opts->key;
    f->text.length = opts->key_len;
  }
  return f;
}

/// The consistent type of the arguments (adding their types); false when there is none.
static bool consistent(const mnode * const * args, size_t n, GTEXT_CSS_Math_Type * out) {
  GTEXT_CSS_Math_Type t = args[0]->type;
  for (size_t i = 1; i < n; i++) {
    if (!css_math_type_add(&t, &args[i]->type, &t)) {
      return false;
    }
  }
  *out = t;
  return true;
}

/// Radians or degrees to the other.
static double to_radians(double deg) {
  return rnd(rnd(deg * PI_VALUE) / 180.0);
}

static double to_degrees(double rad) {
  return rnd(rnd(rad * 180.0) / PI_VALUE);
}

/// min() or max() with the numeric leaves of one unit (not percentages) combined.
static const mnode * min_max_partial(mbuilder * mb, GTEXT_CSS_Math_Op op, const mnode * const * args, size_t n,
    const GTEXT_CSS_Math_Type * type) {
  const GTEXT_Allocator * a = &mb->alloc;
  const mnode ** out = gtext_allocator_malloc(a, n * sizeof *out);
  double * vals = gtext_allocator_malloc(a, n * sizeof *vals);
  int32_t * slot = gtext_allocator_malloc(a, GROUPS * sizeof *slot);
  unsigned char * mf = gtext_allocator_calloc(a, n, 1);
  const mnode * result = NULL;
  if (!out || !vals || !slot || !mf) {
    mb_fail(mb, GTEXT_CSS_E_OOM);
    goto done;
  }
  for (size_t i = 0; i < GROUPS; i++) {
    slot[i] = -1;
  }
  size_t m = 0;
  for (size_t i = 0; i < n; i++) {
    const mnode * c = args[i];
    mb->work++;
    if (css_math_is_numeric(c) && c->op != GTEXT_CSS_MATH_PERCENT) {
      const size_t key = group_key(c);
      if (slot[key] >= 0) {
        const size_t s = (size_t)slot[key];
        vals[s] = op == GTEXT_CSS_MATH_MIN ? min2(vals[s], c->value) : max2(vals[s], c->value);
        mf[s] = 1;
        continue;
      }
      slot[key] = (int32_t)m;
      vals[m] = c->value;
    }
    out[m++] = c;
  }
  for (size_t i = 0; i < m; i++) {
    if (mf[i]) {
      out[i] = leaf_like(mb, out[i], vals[i]);
      if (!out[i]) {
        goto done;
      }
    }
  }
  if (m == 1) {
    result = out[0];
    goto done;
  }
  result = fn_node(mb, op, out, m, type, &(const mfn_opts){GTEXT_CSS_MATH_ROUND_NEAREST, false, false, NULL, 0});
done:
  gtext_allocator_free(a, out);
  gtext_allocator_free(a, vals);
  gtext_allocator_free(a, slot);
  gtext_allocator_free(a, mf);
  return result;
}

const mnode * mk_fn(mbuilder * mb, GTEXT_CSS_Math_Op op, const mnode * const * args, size_t n, const mfn_opts * opts) {
  static const mfn_opts none = {GTEXT_CSS_MATH_ROUND_NEAREST, false, false, NULL, 0};
  if (!opts) {
    opts = &none;
  }
  GTEXT_CSS_Math_Round round = opts->round;
  GTEXT_CSS_Math_Type type;
  memset(&type, 0, sizeof type);
  mb->work++;
  /* clamp() with a side that is none is a min() or a max() (or just the value) */
  if (op == GTEXT_CSS_MATH_CLAMP) {
    if (n != 3 || !args[1]) {
      mb_invalid(mb);
      return NULL;
    }
    if (!args[0] && !args[2]) {
      return args[1];
    }
    if (!args[0]) {
      const mnode * two[2] = {args[1], args[2]};
      return mk_fn(mb, GTEXT_CSS_MATH_MIN, two, 2, NULL);
    }
    if (!args[2]) {
      const mnode * two[2] = {args[0], args[1]};
      return mk_fn(mb, GTEXT_CSS_MATH_MAX, two, 2, NULL);
    }
  }
  for (size_t i = 0; i < n; i++) {
    if (!args[i]) {
      mb_invalid(mb);
      return NULL;
    }
  }
  switch (op) {
  case GTEXT_CSS_MATH_MIN: case GTEXT_CSS_MATH_MAX: case GTEXT_CSS_MATH_HYPOT:
    if (n < 1 || !consistent(args, n, &type) || css_math_type_category(&type) == GTEXT_CSS_MATH_CAT_NONE) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  case GTEXT_CSS_MATH_CLAMP:
    if (!consistent(args, 3, &type) || css_math_type_category(&type) == GTEXT_CSS_MATH_CAT_NONE) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  case GTEXT_CSS_MATH_ROUND:
    if (n == 1) {
      const GTEXT_CSS_Math_Category c = css_math_type_category(&args[0]->type);
      if (round == GTEXT_CSS_MATH_ROUND_LINE_WIDTH ? c != GTEXT_CSS_MATH_CAT_LENGTH : c != GTEXT_CSS_MATH_CAT_NUMBER) {
        mb_invalid(mb);
        return NULL;
      }
      type = args[0]->type;
    }
    else if (n == 2) {
      if (!consistent(args, 2, &type) || css_math_type_category(&type) == GTEXT_CSS_MATH_CAT_NONE ||
          (round == GTEXT_CSS_MATH_ROUND_LINE_WIDTH && css_math_type_category(&args[0]->type) != GTEXT_CSS_MATH_CAT_LENGTH)) {
        mb_invalid(mb);
        return NULL;
      }
    }
    else {
      mb_invalid(mb);
      return NULL;
    }
    break;
  case GTEXT_CSS_MATH_MOD: case GTEXT_CSS_MATH_REM:
    if (n != 2 || !consistent(args, 2, &type) || css_math_type_category(&type) == GTEXT_CSS_MATH_CAT_NONE) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  case GTEXT_CSS_MATH_SIN: case GTEXT_CSS_MATH_COS: case GTEXT_CSS_MATH_TAN: {
    if (n != 1) {
      mb_invalid(mb);
      return NULL;
    }
    const GTEXT_CSS_Math_Category c = css_math_type_category(&args[0]->type);
    if (c != GTEXT_CSS_MATH_CAT_NUMBER && c != GTEXT_CSS_MATH_CAT_ANGLE) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  }
  case GTEXT_CSS_MATH_ASIN: case GTEXT_CSS_MATH_ACOS: case GTEXT_CSS_MATH_ATAN:
    if (n != 1 || css_math_type_category(&args[0]->type) != GTEXT_CSS_MATH_CAT_NUMBER) {
      mb_invalid(mb);
      return NULL;
    }
    type = gtext_css_math_category_type(GTEXT_CSS_MATH_CAT_ANGLE);
    break;
  case GTEXT_CSS_MATH_ATAN2: {
    GTEXT_CSS_Math_Type t;
    if (n != 2 || !consistent(args, 2, &t) || css_math_type_category(&t) == GTEXT_CSS_MATH_CAT_NONE) {
      mb_invalid(mb);
      return NULL;
    }
    type = gtext_css_math_category_type(GTEXT_CSS_MATH_CAT_ANGLE);
    break;
  }
  case GTEXT_CSS_MATH_POW:
    if (n != 2 || css_math_type_category(&args[0]->type) != GTEXT_CSS_MATH_CAT_NUMBER ||
        css_math_type_category(&args[1]->type) != GTEXT_CSS_MATH_CAT_NUMBER) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  case GTEXT_CSS_MATH_SQRT: case GTEXT_CSS_MATH_EXP:
    if (n != 1 || css_math_type_category(&args[0]->type) != GTEXT_CSS_MATH_CAT_NUMBER) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  case GTEXT_CSS_MATH_LOG:
    if ((n != 1 && n != 2) || css_math_type_category(&args[0]->type) != GTEXT_CSS_MATH_CAT_NUMBER ||
        (n == 2 && css_math_type_category(&args[1]->type) != GTEXT_CSS_MATH_CAT_NUMBER)) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  case GTEXT_CSS_MATH_ABS:
    if (n != 1) {
      mb_invalid(mb);
      return NULL;
    }
    type = args[0]->type;
    break;
  case GTEXT_CSS_MATH_RANDOM:
    if ((n != 2 && n != 3) || !consistent(args, n, &type) || css_math_type_category(&type) == GTEXT_CSS_MATH_CAT_NONE) {
      mb_invalid(mb);
      return NULL;
    }
    return fn_node(mb, op, args, n, &type, opts);
  case GTEXT_CSS_MATH_PROGRESS: {
    GTEXT_CSS_Math_Type t;
    if (n != 3 || !consistent(args, 3, &t) || css_math_type_category(&t) == GTEXT_CSS_MATH_CAT_NONE) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  }
  case GTEXT_CSS_MATH_SIGN:
    if (n != 1) {
      mb_invalid(mb);
      return NULL;
    }
    break;
  default:
    mb_invalid(mb);
    return NULL;
  }
  /* compute it when every argument is a number or an absolute quantity */
  bool all = true;
  for (size_t i = 0; i < n; i++) {
    if (!foldable(args[i])) {
      all = false;
      break;
    }
  }
  if (all && !(op == GTEXT_CSS_MATH_ROUND && round == GTEXT_CSS_MATH_ROUND_LINE_WIDTH)) {
    double v[64];
    double * vp = v;
    double * heap = NULL;
    if (n > sizeof v / sizeof v[0]) {
      heap = gtext_allocator_malloc(&mb->alloc, n * sizeof *heap);
      if (!heap) {
        mb_fail(mb, GTEXT_CSS_E_OOM);
        return NULL;
      }
      vp = heap;
    }
    for (size_t i = 0; i < n; i++) {
      vp[i] = args[i]->value;
    }
    if ((op == GTEXT_CSS_MATH_SIN || op == GTEXT_CSS_MATH_COS || op == GTEXT_CSS_MATH_TAN) && args[0]->op == GTEXT_CSS_MATH_DIMENSION) {
      vp[0] = to_radians(vp[0]);
    }
    double r;
    const bool ok = css_math_apply_ex(op, round, opts->no_clamp, vp, n, &r);
    gtext_allocator_free(&mb->alloc, heap);
    if (ok) {
      if (op == GTEXT_CSS_MATH_ASIN || op == GTEXT_CSS_MATH_ACOS || op == GTEXT_CSS_MATH_ATAN || op == GTEXT_CSS_MATH_ATAN2) {
        r = to_degrees(r);
        return mk_dimension(mb, r, GTEXT_CSS_UNIT_DEG);
      }
      const GTEXT_CSS_Math_Category cat = css_math_type_category(&type);
      if (cat != GTEXT_CSS_MATH_CAT_NONE && cat != GTEXT_CSS_MATH_CAT_PERCENTAGE) {
        return leaf_of_category(mb, cat, r);
      }
    }
  }
  if (op == GTEXT_CSS_MATH_MIN || op == GTEXT_CSS_MATH_MAX) {
    return min_max_partial(mb, op, args, n, &type);
  }
  {
    mfn_opts used = *opts;
    used.round = round;
    return fn_node(mb, op, args, n, &type, &used);
  }
}
