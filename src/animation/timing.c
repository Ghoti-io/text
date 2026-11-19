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
 * The timing model of an animation effect (Web Animations 1, "Timing model") as a function of the effect's timing and a local time, and the dictionaries `EffectTiming` and
 * `OptionalEffectTiming` (validation and the delta update). The arithmetic is IEEE double through the math library; an infinity is made by arithmetic on a volatile zero and tested by
 * comparison, as the rest of the library does (no <math.h>).
 */

#include <ghoti.io/math/basic.h>
#include <ghoti.io/text/animation/timing.h>
#include <ghoti.io/text/css/css_animation.h>
#include <ghoti.io/text/css/css_property.h>

#include <string.h>

/* ---- numbers ------------------------------------------------------------------------------------------------------------------------------------ */

static double infinity(void) {
  volatile double zero = 0.0;
  return 1.0 / zero;
}

static bool is_nan(double x) {
  return x != x;
}

static bool is_infinite(double x) {
  return x == infinity() || x == -infinity();
}

static bool is_finite(double x) {
  return !is_nan(x) && !is_infinite(x);
}

static double max2(double a, double b) {
  return a > b ? a : b;
}

static double min2(double a, double b) {
  return a < b ? a : b;
}

/* ---- EffectTiming ------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_Anim_Effect_Timing gtext_anim_timing_default(void) {
  GTEXT_Anim_Effect_Timing t;
  memset(&t, 0, sizeof t);
  t.fill = GTEXT_ANIM_FILL_AUTO;
  t.iterations = 1.0;
  t.duration_auto = true;
  t.direction = GTEXT_ANIM_DIRECTION_NORMAL;
  return t;
}

GTEXT_Anim_Status gtext_anim_timing_validate(const GTEXT_Anim_Effect_Timing * t) {
  if (!t) {
    return GTEXT_ANIM_INVALID;
  }
  /* the members of type `double` of the IDL are finite; the conversion that rejects the others is the binding's, and the same refusal is here so that no value a caller can build is outside the model */
  if (!is_finite(t->delay) || !is_finite(t->end_delay) || !is_finite(t->iteration_start)) {
    return GTEXT_ANIM_TYPE_ERROR;
  }
  /* "Updating the timing of an AnimationEffect": iterationStart less than zero; iterations less than zero or NaN; duration less than zero or NaN */
  if (t->iteration_start < 0.0 || t->iterations < 0.0 || is_nan(t->iterations)) {
    return GTEXT_ANIM_TYPE_ERROR;
  }
  if (!t->duration_auto && (t->duration < 0.0 || is_nan(t->duration))) {
    return GTEXT_ANIM_TYPE_ERROR;
  }
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_timing_update(GTEXT_Anim_Effect_Timing * timing, const GTEXT_Anim_Optional_Timing * input) {
  if (!timing || !input) {
    return GTEXT_ANIM_INVALID;
  }
  /* the checks are made on the members that exist, in the order of the specification, before anything is assigned */
  GTEXT_Anim_Effect_Timing next = *timing;
  const GTEXT_Anim_Effect_Timing * v = &input->values;
  if (input->present & GTEXT_ANIM_TIMING_DELAY) {
    next.delay = v->delay;
  }
  if (input->present & GTEXT_ANIM_TIMING_END_DELAY) {
    next.end_delay = v->end_delay;
  }
  if (input->present & GTEXT_ANIM_TIMING_FILL) {
    next.fill = v->fill;
  }
  if (input->present & GTEXT_ANIM_TIMING_ITERATION_START) {
    next.iteration_start = v->iteration_start;
  }
  if (input->present & GTEXT_ANIM_TIMING_ITERATIONS) {
    next.iterations = v->iterations;
  }
  if (input->present & GTEXT_ANIM_TIMING_DURATION) {
    next.duration_auto = v->duration_auto;
    next.duration = v->duration_auto ? 0.0 : v->duration;
  }
  if (input->present & GTEXT_ANIM_TIMING_DIRECTION) {
    next.direction = v->direction;
  }
  if (input->present & GTEXT_ANIM_TIMING_EASING) {
    next.easing = v->easing;
  }
  /* a member that does not exist keeps the value the effect has, which is valid, so validating the result is validating the members that exist */
  const GTEXT_Anim_Status st = gtext_anim_timing_validate(&next);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  *timing = next;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_easing_parse(const GTEXT_CSS_Registry * registry, const char * text, size_t length, const GTEXT_Allocator * allocator, GTEXT_CSS_Easing ** out) {
  if (!registry || !out || (!text && length)) {
    return GTEXT_ANIM_INVALID;
  }
  GTEXT_CSS_Property_Value value;
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = allocator;
  const GTEXT_CSS_Status ps = gtext_css_parse_property_text(registry, "transition-timing-function", 26, text ? text : "", length, &po, &value);
  if (ps == GTEXT_CSS_E_OOM) {
    return GTEXT_ANIM_OOM;
  }
  if (ps != GTEXT_CSS_OK) {
    return ps == GTEXT_CSS_E_INVALID ? GTEXT_ANIM_INVALID : GTEXT_ANIM_TYPE_ERROR;
  }
  GTEXT_Anim_Status result = GTEXT_ANIM_TYPE_ERROR;
  const GTEXT_CSS_Value * root = value.outcome == GTEXT_CSS_PROP_RESULT_VALID && value.set ? gtext_css_value_set_root(value.set) : NULL;
  /* one easing function: a list of them is a valid `transition-timing-function` and not an `<easing-function>` (a list of one is the value of the property for one function) */
  const GTEXT_CSS_Value * one = root;
  if (root && root->kind == GTEXT_CSS_VAL_LIST && root->separator == GTEXT_CSS_SEP_COMMA) {
    one = root->child_count == 1 ? &root->children[0] : NULL;
  }
  if (one) {
    bool valid = false;
    GTEXT_CSS_Easing * easing = NULL;
    const GTEXT_CSS_Status es = gtext_css_easing_from_value(one, allocator, &valid, &easing);
    if (es == GTEXT_CSS_E_OOM) {
      result = GTEXT_ANIM_OOM;
    }
    else if (es == GTEXT_CSS_OK && valid) {
      *out = easing;
      result = GTEXT_ANIM_OK;
    }
  }
  gtext_css_property_value_free(&value);
  return result;
}

/* ---- the timing model --------------------------------------------------------------------------------------------------------------------------- */

GTEXT_Anim_Status gtext_anim_computed_timing(const GTEXT_Anim_Effect_Timing * t, GTEXT_Anim_Time local, double playback_rate, bool endpoint_inclusive, GTEXT_Anim_Computed_Timing * out) {
  if (!t || !out) {
    return GTEXT_ANIM_INVALID;
  }
  memset(out, 0, sizeof *out);
  const double inf = infinity();
  out->timing = *t;
  if (out->timing.fill == GTEXT_ANIM_FILL_AUTO) {
    out->timing.fill = GTEXT_ANIM_FILL_NONE;
  }
  if (out->timing.duration_auto) {
    out->timing.duration_auto = false;
    out->timing.duration = 0.0;
  }
  const double dur = out->timing.duration;
  const double count = t->iterations;
  /* "active duration = iteration duration x iteration count. If either ... are zero, the active duration is zero." */
  const double active_duration = (dur == 0.0 || count == 0.0) ? 0.0 : dur * count;
  const double delay = t->delay;
  const double end_time = max2(delay + active_duration + t->end_delay, 0.0);
  out->active_duration = active_duration;
  out->end_time = end_time;
  out->local_time = local;
  if (!local.resolved) {
    return GTEXT_ANIM_OK;   /* in none of the phases: the active time, and with it every progress, is unresolved */
  }
  const double lt = local.ms;
  const double before_boundary = max2(min2(delay, end_time), 0.0);
  const double after_boundary = max2(min2(delay + active_duration, end_time), 0.0);
  const bool backwards = playback_rate < 0.0;
  if (lt < before_boundary || (backwards && !endpoint_inclusive && lt == before_boundary)) {
    out->phase = GTEXT_ANIM_PHASE_BEFORE;
  }
  else if (lt > after_boundary || (!backwards && !endpoint_inclusive && lt == after_boundary)) {
    out->phase = GTEXT_ANIM_PHASE_AFTER;
  }
  else {
    out->phase = GTEXT_ANIM_PHASE_ACTIVE;
  }
  /* "Calculating the active time" */
  const GTEXT_Anim_Fill fill = out->timing.fill;
  double active;
  switch (out->phase) {
    case GTEXT_ANIM_PHASE_BEFORE:
      if (fill != GTEXT_ANIM_FILL_BACKWARDS && fill != GTEXT_ANIM_FILL_BOTH) {
        return GTEXT_ANIM_OK;
      }
      active = max2(lt - delay, 0.0);
      break;
    case GTEXT_ANIM_PHASE_ACTIVE:
      active = lt - delay;
      break;
    default:
      if (fill != GTEXT_ANIM_FILL_FORWARDS && fill != GTEXT_ANIM_FILL_BOTH) {
        return GTEXT_ANIM_OK;
      }
      active = max2(min2(lt - delay, active_duration), 0.0);
      break;
  }
  out->in_effect = true;
  out->active_time = active;
  /* "Calculating the overall progress" */
  double overall;
  if (dur == 0.0) {
    overall = out->phase == GTEXT_ANIM_PHASE_BEFORE ? 0.0 : count;
  }
  else {
    overall = active / dur;
  }
  overall += t->iteration_start;
  out->overall_progress = overall;
  /* "Calculating the simple iteration progress" */
  const bool overall_infinite = overall == inf;
  double simple = overall_infinite ? t->iteration_start - gmath_floor(t->iteration_start) : overall - gmath_floor(overall);
  if (simple == 0.0 && out->phase != GTEXT_ANIM_PHASE_BEFORE && active == active_duration && count != 0.0) {
    simple = 1.0;
  }
  out->simple_progress = simple;
  /* "Calculating the current iteration index" */
  out->has_current_iteration = true;
  if (out->phase == GTEXT_ANIM_PHASE_AFTER && count == inf) {
    out->current_iteration = inf;   /* the same number floor(infinity) - 1 gives; written as the text writes it */
  }
  else if (simple == 1.0) {
    out->current_iteration = gmath_floor(overall) - 1.0;
  }
  else {
    out->current_iteration = gmath_floor(overall);
  }
  /* "Calculating the directed progress" */
  bool forwards;
  switch (t->direction) {
    case GTEXT_ANIM_DIRECTION_NORMAL:
      forwards = true;
      break;
    case GTEXT_ANIM_DIRECTION_REVERSE:
      forwards = false;
      break;
    default: {
      double d = out->current_iteration;
      if (d == inf) {
        forwards = true;
        break;
      }
      if (t->direction == GTEXT_ANIM_DIRECTION_ALTERNATE_REVERSE) {
        d += 1.0;
      }
      forwards = d - 2.0 * gmath_floor(d / 2.0) == 0.0;
      break;
    }
  }
  out->forwards = forwards;
  out->directed_progress = forwards ? simple : 1.0 - simple;
  /* "Calculating the transformed progress" */
  out->before_flag = (out->phase == GTEXT_ANIM_PHASE_BEFORE && forwards) || (out->phase == GTEXT_ANIM_PHASE_AFTER && !forwards);
  out->progress = gtext_css_easing_output(t->easing, out->directed_progress, out->before_flag);
  out->has_progress = true;
  return GTEXT_ANIM_OK;
}

bool gtext_anim_effect_is_relevant(const GTEXT_Anim_Computed_Timing * c, GTEXT_Anim_Play_State play_state, double playback_rate) {
  if (!c) {
    return false;
  }
  const bool in_play = c->phase == GTEXT_ANIM_PHASE_ACTIVE && play_state != GTEXT_ANIM_PLAY_FINISHED;
  const bool current = in_play || (c->phase == GTEXT_ANIM_PHASE_BEFORE && playback_rate > 0.0) || (c->phase == GTEXT_ANIM_PHASE_AFTER && playback_rate < 0.0);
  return current || c->in_effect;
}
