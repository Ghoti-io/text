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
 * The timing model of an effect (Web Animations 1, "Timing model") and the output of an easing function (CSS Easing Functions 1 and 2).
 *
 * An infinity is never made: an `infinite` count is a flag, and a time that would be infinite is a sentinel larger than any time a caller can name.
 */

#include <ghoti.io/math/basic.h>
#include <ghoti.io/text/css/css_animation.h>

/* ---- easing functions ----------------------------------------------------------------------------------------------------------------------------- */

/// x(t) of the Bezier with P0 = (0, 0), P1 = (x1, y1), P2 = (x2, y2), P3 = (1, 1), written as a cubic in t.
static double bezier(double a, double b, double t) {
  const double u = 1.0 - t;
  return 3.0 * u * u * t * a + 3.0 * u * t * t * b + t * t * t;
}

static double bezier_slope(double a, double b, double t) {
  const double u = 1.0 - t;
  return 3.0 * u * u * a + 6.0 * u * t * (b - a) + 3.0 * t * t * (1.0 - b);
}

static double cubic_output(const GTEXT_CSS_Easing * e, double x) {
  if (x < 0.0) {
    if (e->x1 > 0.0) {
      return x * (e->y1 / e->x1);
    }
    if (e->x2 > 0.0) {
      return x * (e->y2 / e->x2);
    }
    return 0.0;
  }
  if (x > 1.0) {
    if (e->x2 < 1.0) {
      return 1.0 + (x - 1.0) * ((1.0 - e->y2) / (1.0 - e->x2));
    }
    if (e->x1 < 1.0) {
      return 1.0 + (x - 1.0) * ((1.0 - e->y1) / (1.0 - e->x1));
    }
    return 1.0;
  }
  /* x(t) is monotonic on [0, 1] (both x control points are in [0, 1]): Newton's method from the input, then bisection if it does not settle */
  double t = x;
  for (int i = 0; i < 8; i++) {
    const double err = bezier(e->x1, e->x2, t) - x;
    if (err < 1e-12 && err > -1e-12) {
      return bezier(e->y1, e->y2, t);
    }
    const double d = bezier_slope(e->x1, e->x2, t);
    if (d < 1e-9 && d > -1e-9) {
      break;
    }
    t -= err / d;
    if (t < 0.0 || t > 1.0) {
      break;
    }
  }
  double lo = 0.0, hi = 1.0;
  t = x;
  for (int i = 0; i < 64; i++) {
    const double v = bezier(e->x1, e->x2, t);
    if (v < x) {
      lo = t;
    }
    else {
      hi = t;
    }
    t = (lo + hi) * 0.5;
  }
  return bezier(e->y1, e->y2, t);
}

static double steps_output(const GTEXT_CSS_Easing * e, double x, bool before) {
  const double n = (double)e->steps;
  double step = gmath_floor(x * n);
  if (e->position == GTEXT_CSS_STEP_JUMP_START || e->position == GTEXT_CSS_STEP_JUMP_BOTH) {
    step += 1.0;
  }
  const double scaled = x * n;
  if (before && scaled == gmath_floor(scaled)) {
    step -= 1.0;
  }
  if (x >= 0.0 && step < 0.0) {
    step = 0.0;
  }
  double jumps = n;
  if (e->position == GTEXT_CSS_STEP_JUMP_NONE) {
    jumps = n - 1.0;
  }
  else if (e->position == GTEXT_CSS_STEP_JUMP_BOTH) {
    jumps = n + 1.0;
  }
  if (x <= 1.0 && step > jumps) {
    step = jumps;
  }
  return step / jumps;
}

static double points_output(const GTEXT_CSS_Easing * e, double x, bool before) {
  const GTEXT_CSS_Easing_Point * p = e->points;
  const size_t n = e->point_count;
  if (n == 0) {
    return x;
  }
  if (n == 1) {
    return p[0].output;
  }
  if (x == p[0].input && before) {
    return p[0].output;
  }
  for (size_t i = n; i-- > 0;) {
    if (p[i].input == x) {
      return p[i].output;   /* the last point at that input */
    }
  }
  size_t a, b;
  if (x < p[0].input) {
    if (p[0].input == p[1].input) {
      return p[0].output;
    }
    a = 0;
    b = 1;
  }
  else if (x > p[n - 1].input) {
    if (p[n - 2].input == p[n - 1].input) {
      return p[n - 1].output;
    }
    a = n - 2;
    b = n - 1;
  }
  else {
    a = 0;
    for (size_t i = 0; i < n; i++) {
      if (p[i].input < x) {
        a = i;
      }
    }
    b = a + 1;
    while (b < n && p[b].input <= x) {
      b++;
    }
    if (b >= n) {
      b = n - 1;
    }
  }
  return p[a].output + (x - p[a].input) * ((p[b].output - p[a].output) / (p[b].input - p[a].input));
}

double gtext_css_easing_output(const GTEXT_CSS_Easing * easing, double input, bool before_flag) {
  if (!easing) {
    return input;
  }
  switch (easing->kind) {
    case GTEXT_CSS_EASING_LINEAR:
      return input;
    case GTEXT_CSS_EASING_LINEAR_POINTS:
      return points_output(easing, input, before_flag);
    case GTEXT_CSS_EASING_CUBIC_BEZIER:
      return cubic_output(easing, input);
    case GTEXT_CSS_EASING_STEPS:
      return steps_output(easing, input, before_flag);
  }
  return input;
}

/* ---- the timing model ----------------------------------------------------------------------------------------------------------------------------- */

#define BIG 1e300   /* the sentinel for "never": larger than any time */

static double max2(double a, double b) {
  return a > b ? a : b;
}

static double min2(double a, double b) {
  return a < b ? a : b;
}

void gtext_css_anim_timing(const GTEXT_CSS_Anim_Effect * effect, double local, GTEXT_CSS_Anim_Timing * out) {
  *out = (GTEXT_CSS_Anim_Timing){0};
  const double dur = effect->duration_auto ? 0.0 : effect->duration_ms;
  const bool zero_count = !effect->infinite && effect->iteration_count == 0.0;
  /* "active duration = iteration duration x iteration count. If either ... are zero, the active duration is zero." */
  double active_duration;
  if (dur == 0.0 || zero_count) {
    active_duration = 0.0;
  }
  else if (effect->infinite) {
    active_duration = BIG;
  }
  else {
    active_duration = dur * effect->iteration_count;
  }
  const double delay = effect->delay_start_ms;
  const double end_time = max2(delay + active_duration + effect->delay_end_ms, 0.0);
  const double before_boundary = max2(min2(delay, end_time), 0.0);
  const double after_boundary = max2(min2(delay + active_duration, end_time), 0.0);
  /* the animation direction is forward (a playback rate of 1), and the endpoint-inclusive flag is false */
  if (local < before_boundary) {
    out->phase = GTEXT_CSS_ANIM_PHASE_BEFORE;
  }
  else if (local >= after_boundary) {
    out->phase = GTEXT_CSS_ANIM_PHASE_AFTER;
  }
  else {
    out->phase = GTEXT_CSS_ANIM_PHASE_ACTIVE;
  }
  /* "Calculating the active time" */
  double active;
  switch (out->phase) {
    case GTEXT_CSS_ANIM_PHASE_BEFORE:
      if (effect->fill != GTEXT_CSS_ANIM_FILL_BACKWARDS && effect->fill != GTEXT_CSS_ANIM_FILL_BOTH) {
        return;
      }
      active = max2(local - delay, 0.0);
      break;
    case GTEXT_CSS_ANIM_PHASE_ACTIVE:
      active = local - delay;
      break;
    default:
      if (effect->fill != GTEXT_CSS_ANIM_FILL_FORWARDS && effect->fill != GTEXT_CSS_ANIM_FILL_BOTH) {
        return;
      }
      active = max2(min2(local - delay, active_duration), 0.0);
      break;
  }
  out->in_effect = true;
  out->active_time_ms = active;
  /* "Calculating the overall progress" */
  double overall;
  if (dur == 0.0) {
    if (out->phase == GTEXT_CSS_ANIM_PHASE_BEFORE) {
      overall = 0.0;
    }
    else if (effect->infinite) {
      overall = 0.0;
      out->overall_infinite = true;
    }
    else {
      overall = effect->iteration_count;
    }
  }
  else {
    overall = active / dur;
  }
  out->overall_progress = overall;
  /* "Calculating the simple iteration progress" (the iteration start is 0) */
  double simple = out->overall_infinite ? 0.0 : overall - gmath_floor(overall);
  const bool count_nonzero = effect->infinite || effect->iteration_count != 0.0;
  if (simple == 0.0 && out->phase != GTEXT_CSS_ANIM_PHASE_BEFORE && active == active_duration && count_nonzero) {
    simple = 1.0;
  }
  out->simple_progress = simple;
  /* "Calculating the current iteration index" */
  if (out->phase == GTEXT_CSS_ANIM_PHASE_AFTER && effect->infinite) {
    out->iteration_infinite = true;
  }
  else if (simple == 1.0) {
    out->iteration_index = (out->overall_infinite ? 0.0 : gmath_floor(overall)) - 1.0;
  }
  else {
    out->iteration_index = out->overall_infinite ? 0.0 : gmath_floor(overall);
  }
  /* "Calculating the directed progress" */
  bool forwards;
  switch (effect->direction) {
    case GTEXT_CSS_ANIM_DIRECTION_NORMAL:
      forwards = true;
      break;
    case GTEXT_CSS_ANIM_DIRECTION_REVERSE:
      forwards = false;
      break;
    default: {
      if (out->iteration_infinite) {
        forwards = true;
        break;
      }
      double d = out->iteration_index;
      if (effect->direction == GTEXT_CSS_ANIM_DIRECTION_ALTERNATE_REVERSE) {
        d += 1.0;
      }
      forwards = d - 2.0 * gmath_floor(d / 2.0) == 0.0;
      break;
    }
  }
  out->forwards = forwards;
  out->progress = forwards ? simple : 1.0 - simple;
  out->before_flag = (out->phase == GTEXT_CSS_ANIM_PHASE_BEFORE && forwards) || (out->phase == GTEXT_CSS_ANIM_PHASE_AFTER && !forwards);
}
