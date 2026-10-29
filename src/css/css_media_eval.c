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
 * The media environment, the evaluation of media features and the three-valued evaluator of media query lists (Media Queries 4, 3.1). The
 * evaluator is a machine with its own stack of frames: a query, a `not`, an `and` list and a custom media definition are frames, so the depth of
 * a condition and the length of a chain of `@custom-media` are bounded by memory and by `max_work`, never by the C stack.
 */

#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_math.h>
#include <ghoti.io/text/css/css_media.h>
#include <ghoti.io/text/css/css_value.h>

#include <ghoti.io/math/math.h>

#include <string.h>

#include "css_condition_internal.h"
#include "css_media_internal.h"

/* ---- the environment ----------------------------------------------------- */

static void set_bit(GTEXT_CSS_Media_Environment * env, int row, bool on) {
  if (row < 0 || row >= 128) {
    return;
  }
  if (on) {
    env->supported[row / 32] |= (uint32_t)1 << (row % 32);
  } else {
    env->supported[row / 32] &= ~((uint32_t)1 << (row % 32));
  }
}

static bool get_bit(const GTEXT_CSS_Media_Environment * env, int row) {
  return row >= 0 && row < 128 && (env->supported[row / 32] >> (row % 32) & 1u);
}

bool gtext_css_media_environment_set_supported(GTEXT_CSS_Media_Environment * env, const char * name, bool supported) {
  if (!env || !name) {
    return false;
  }
  int row = css_mq_find_feature(name, strlen(name));
  if (row < 0) {
    return false;
  }
  set_bit(env, row, supported);
  return true;
}

bool gtext_css_media_environment_is_supported(const GTEXT_CSS_Media_Environment * env, const char * name) {
  if (!env || !name) {
    return false;
  }
  return get_bit(env, css_mq_find_feature(name, strlen(name)));
}

GTEXT_CSS_Media_Environment gtext_css_media_environment_default(const GTEXT_CSS_Environment * base) {
  GTEXT_CSS_Environment b = base ? *base : gtext_css_environment_default();
  GTEXT_CSS_Media_Environment e;
  memset(&e, 0, sizeof e);
  e.size = sizeof e;
  e.medium = GTEXT_CSS_MEDIUM_SCREEN;
  e.viewport = b.dynamic_viewport;
  e.device = b.dynamic_viewport;
  e.resolution = b.device_pixel_ratio;
  e.default_font_size = b.default_font_size;
  e.color_bits = 8;
  e.color_gamut = GTEXT_CSS_MQ_GAMUT_SRGB;
  e.video_color_gamut = GTEXT_CSS_MQ_GAMUT_SRGB;
  e.dynamic_range = GTEXT_CSS_MQ_RANGE_STANDARD;
  e.video_dynamic_range = GTEXT_CSS_MQ_RANGE_STANDARD;
  e.scan = GTEXT_CSS_MQ_SCAN_PROGRESSIVE;
  e.update = GTEXT_CSS_MQ_UPDATE_FAST;
  e.overflow_block = GTEXT_CSS_MQ_OVERFLOW_BLOCK_SCROLL;
  e.overflow_inline = GTEXT_CSS_MQ_OVERFLOW_INLINE_SCROLL;
  e.pointer = GTEXT_CSS_MQ_POINTER_FINE;
  e.hover = GTEXT_CSS_MQ_HOVER_HOVER;
  e.any_pointer = GTEXT_CSS_MQ_ANY_POINTER_FINE;
  e.any_hover = GTEXT_CSS_MQ_ANY_HOVER_HOVER;
  e.display_mode = GTEXT_CSS_MQ_DISPLAY_MODE_BROWSER;
  e.horizontal_viewport_segments = 1;
  e.vertical_viewport_segments = 1;
  e.scripting = GTEXT_CSS_MQ_SCRIPTING_ENABLED;
  e.prefers_color_scheme = b.color_scheme == GTEXT_CSS_SCHEME_DARK ? GTEXT_CSS_MQ_SCHEME_DARK : GTEXT_CSS_MQ_SCHEME_LIGHT;
  for (int r = 0; r < CSS_MQF_COUNT; r++) {
    switch ((css_mq_feature_id)r) {
    /* What a plain screen cannot say without the caller: the window, a round display, a back button, blending. */
    case CSS_MQF_DISPLAY_STATE:
    case CSS_MQF_RESIZABLE:
    case CSS_MQF_NAV_CONTROLS:
    case CSS_MQF_SHAPE:
    case CSS_MQF_ENVIRONMENT_BLENDING:
    case CSS_MQF_UA_COLOR_SCHEME:
    case CSS_MQF_COUNT:
      break;
    default:
      set_bit(&e, r, true);
      break;
    }
  }
  return e;
}

static bool fp(double v) {
  return v > 0.0 && !mq_isinf(v);
}

GTEXT_CSS_Status gtext_css_media_environment_check(const GTEXT_CSS_Media_Environment * env, const char ** detail) {
  const char * why = NULL;
  if (!env) {
    if (detail) {
      *detail = "no environment";
    }
    return GTEXT_CSS_E_INVALID;
  }
  if (env->size < offsetof(GTEXT_CSS_Media_Environment, default_font_size) + sizeof env->default_font_size) {
    why = "size does not cover the sizes, the resolution and the default font size";
  } else if (env->medium > GTEXT_CSS_MEDIUM_SPEECH) {
    why = "medium is not a medium";
  } else if (!fp(env->viewport.width) || !fp(env->viewport.height)) {
    why = "viewport must be positive and finite";
  } else if (!fp(env->device.width) || !fp(env->device.height)) {
    why = "device must be positive and finite";
  } else if (!env->resolution_infinite && !fp(env->resolution)) {
    why = "resolution must be positive and finite";
  } else if (!fp(env->default_font_size)) {
    why = "default_font_size must be positive and finite";
  } else if (env->size >= sizeof *env) {
    if (env->color_gamut > GTEXT_CSS_MQ_GAMUT_REC2020 || env->video_color_gamut > GTEXT_CSS_MQ_GAMUT_REC2020) {
      why = "a color gamut is not a GTEXT_CSS_Mq_Gamut";
    } else if (env->dynamic_range > GTEXT_CSS_MQ_RANGE_HIGH || env->video_dynamic_range > GTEXT_CSS_MQ_RANGE_HIGH) {
      why = "a dynamic range is not a GTEXT_CSS_Mq_Dynamic_Range";
    } else if (env->scan > GTEXT_CSS_MQ_SCAN_PROGRESSIVE) {
      why = "scan is not a GTEXT_CSS_Mq_Scan";
    } else if (env->update > GTEXT_CSS_MQ_UPDATE_FAST) {
      why = "update is not a GTEXT_CSS_Mq_Update";
    } else if (env->overflow_block > GTEXT_CSS_MQ_OVERFLOW_BLOCK_PAGED) {
      why = "overflow_block is not a GTEXT_CSS_Mq_Overflow_Block";
    } else if (env->overflow_inline > GTEXT_CSS_MQ_OVERFLOW_INLINE_SCROLL) {
      why = "overflow_inline is not a GTEXT_CSS_Mq_Overflow_Inline";
    } else if (env->pointer > GTEXT_CSS_MQ_POINTER_FINE) {
      why = "pointer is not a GTEXT_CSS_Mq_Pointer";
    } else if (env->hover > GTEXT_CSS_MQ_HOVER_HOVER) {
      why = "hover is not a GTEXT_CSS_Mq_Hover";
    } else if (env->any_pointer & ~(GTEXT_CSS_MQ_ANY_POINTER_NONE | GTEXT_CSS_MQ_ANY_POINTER_COARSE | GTEXT_CSS_MQ_ANY_POINTER_FINE)) {
      why = "any_pointer has a bit that is not GTEXT_CSS_MQ_ANY_POINTER_*";
    } else if (env->any_hover & ~(GTEXT_CSS_MQ_ANY_HOVER_NONE | GTEXT_CSS_MQ_ANY_HOVER_HOVER)) {
      why = "any_hover has a bit that is not GTEXT_CSS_MQ_ANY_HOVER_*";
    } else if (env->display_mode > GTEXT_CSS_MQ_DISPLAY_MODE_PICTURE_IN_PICTURE) {
      why = "display_mode is not a GTEXT_CSS_Mq_Display_Mode";
    } else if (env->display_state > GTEXT_CSS_MQ_DISPLAY_STATE_FULLSCREEN) {
      why = "display_state is not a GTEXT_CSS_Mq_Display_State";
    } else if (env->shape > GTEXT_CSS_MQ_SHAPE_ROUND) {
      why = "shape is not a GTEXT_CSS_Mq_Shape";
    } else if (env->environment_blending > GTEXT_CSS_MQ_BLENDING_SUBTRACTIVE) {
      why = "environment_blending is not a GTEXT_CSS_Mq_Blending";
    } else if (env->nav_controls > GTEXT_CSS_MQ_NAV_BACK) {
      why = "nav_controls is not a GTEXT_CSS_Mq_Nav_Controls";
    } else if (env->inverted_colors > GTEXT_CSS_MQ_INVERTED_INVERTED) {
      why = "inverted_colors is not a GTEXT_CSS_Mq_Inverted_Colors";
    } else if (env->scripting > GTEXT_CSS_MQ_SCRIPTING_ENABLED) {
      why = "scripting is not a GTEXT_CSS_Mq_Scripting";
    } else if (env->forced_colors > GTEXT_CSS_MQ_FORCED_COLORS_ACTIVE) {
      why = "forced_colors is not a GTEXT_CSS_Mq_Forced_Colors";
    } else if (env->prefers_color_scheme > GTEXT_CSS_MQ_SCHEME_DARK || env->ua_color_scheme > GTEXT_CSS_MQ_SCHEME_DARK) {
      why = "a color scheme is not a GTEXT_CSS_Mq_Scheme";
    } else if (env->prefers_contrast > GTEXT_CSS_MQ_CONTRAST_CUSTOM) {
      why = "prefers_contrast is not a GTEXT_CSS_Mq_Contrast";
    } else if (env->prefers_reduced_motion > GTEXT_CSS_MQ_REDUCE_REDUCE || env->prefers_reduced_transparency > GTEXT_CSS_MQ_REDUCE_REDUCE ||
               env->prefers_reduced_data > GTEXT_CSS_MQ_REDUCE_REDUCE) {
      why = "a prefers-reduced value is not a GTEXT_CSS_Mq_Reduce";
    }
  }
  if (why) {
    if (detail) {
      *detail = why;
    }
    return GTEXT_CSS_E_INVALID;
  }
  if (detail) {
    *detail = NULL;
  }
  return GTEXT_CSS_OK;
}

/* ---- three-valued logic ---------------------------------------------------- */

GTEXT_CSS_Tristate css_tri_not(GTEXT_CSS_Tristate a) {
  return a == GTEXT_CSS_TRI_TRUE ? GTEXT_CSS_TRI_FALSE : (a == GTEXT_CSS_TRI_FALSE ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_UNKNOWN);
}

GTEXT_CSS_Tristate css_tri_and(GTEXT_CSS_Tristate a, GTEXT_CSS_Tristate b) {
  if (a == GTEXT_CSS_TRI_FALSE || b == GTEXT_CSS_TRI_FALSE) {
    return GTEXT_CSS_TRI_FALSE;
  }
  if (a == GTEXT_CSS_TRI_UNKNOWN || b == GTEXT_CSS_TRI_UNKNOWN) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  return GTEXT_CSS_TRI_TRUE;
}

GTEXT_CSS_Tristate css_tri_or(GTEXT_CSS_Tristate a, GTEXT_CSS_Tristate b) {
  if (a == GTEXT_CSS_TRI_TRUE || b == GTEXT_CSS_TRI_TRUE) {
    return GTEXT_CSS_TRI_TRUE;
  }
  if (a == GTEXT_CSS_TRI_UNKNOWN || b == GTEXT_CSS_TRI_UNKNOWN) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  return GTEXT_CSS_TRI_FALSE;
}

/* ---- values in the environment ------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Media_Environment * env;
  const char * detail;
  GTEXT_CSS_Status status;
} unit_ctx;

static bool unit_px(unit_ctx * u, GTEXT_CSS_Unit unit, double * px) {
  const GTEXT_CSS_Media_Environment * e = u->env;
  if (gtext_css_unit_is_absolute(unit)) {
    return gtext_css_unit_to_canonical(unit, 1.0, px);
  }
  switch (unit) {
  case GTEXT_CSS_UNIT_EM:
  case GTEXT_CSS_UNIT_REM:
    *px = e->default_font_size;
    return true;
  case GTEXT_CSS_UNIT_EX: case GTEXT_CSS_UNIT_REX:
  case GTEXT_CSS_UNIT_CH: case GTEXT_CSS_UNIT_RCH:
  case GTEXT_CSS_UNIT_CAP: case GTEXT_CSS_UNIT_RCAP:
  case GTEXT_CSS_UNIT_IC: case GTEXT_CSS_UNIT_RIC: {
    GTEXT_CSS_Font_Metric m = (unit == GTEXT_CSS_UNIT_EX || unit == GTEXT_CSS_UNIT_REX)   ? GTEXT_CSS_METRIC_EX
                              : (unit == GTEXT_CSS_UNIT_CH || unit == GTEXT_CSS_UNIT_RCH) ? GTEXT_CSS_METRIC_CH
                              : (unit == GTEXT_CSS_UNIT_CAP || unit == GTEXT_CSS_UNIT_RCAP) ? GTEXT_CSS_METRIC_CAP
                                                                                            : GTEXT_CSS_METRIC_IC;
    if (!e->font_metric) {
      u->status = GTEXT_CSS_E_UNSUPPORTED;
      u->detail = gtext_css_unit_name(unit);
      return false;
    }
    GTEXT_CSS_Font_Query q;
    memset(&q, 0, sizeof q);
    q.size_px = e->default_font_size;
    q.weight = 400;
    q.width_percent = 100;
    double px1;
    if (!e->font_metric(e->user, &q, m, &px1)) {
      u->status = GTEXT_CSS_E_UNSUPPORTED;
      u->detail = gtext_css_unit_name(unit);
      return false;
    }
    /* The callback answers in px at the query's size, which is one em. */
    *px = px1;
    return true;
  }
  case GTEXT_CSS_UNIT_VW: case GTEXT_CSS_UNIT_SVW: case GTEXT_CSS_UNIT_LVW: case GTEXT_CSS_UNIT_DVW:
  case GTEXT_CSS_UNIT_VI: case GTEXT_CSS_UNIT_SVI: case GTEXT_CSS_UNIT_LVI: case GTEXT_CSS_UNIT_DVI:
  case GTEXT_CSS_UNIT_CQW: case GTEXT_CSS_UNIT_CQI:
    *px = e->viewport.width / 100.0;
    return true;
  case GTEXT_CSS_UNIT_VH: case GTEXT_CSS_UNIT_SVH: case GTEXT_CSS_UNIT_LVH: case GTEXT_CSS_UNIT_DVH:
  case GTEXT_CSS_UNIT_VB: case GTEXT_CSS_UNIT_SVB: case GTEXT_CSS_UNIT_LVB: case GTEXT_CSS_UNIT_DVB:
  case GTEXT_CSS_UNIT_CQH: case GTEXT_CSS_UNIT_CQB:
    *px = e->viewport.height / 100.0;
    return true;
  case GTEXT_CSS_UNIT_VMIN: case GTEXT_CSS_UNIT_SVMIN: case GTEXT_CSS_UNIT_LVMIN: case GTEXT_CSS_UNIT_DVMIN:
  case GTEXT_CSS_UNIT_CQMIN:
    *px = (e->viewport.width < e->viewport.height ? e->viewport.width : e->viewport.height) / 100.0;
    return true;
  case GTEXT_CSS_UNIT_VMAX: case GTEXT_CSS_UNIT_SVMAX: case GTEXT_CSS_UNIT_LVMAX: case GTEXT_CSS_UNIT_DVMAX:
  case GTEXT_CSS_UNIT_CQMAX:
    *px = (e->viewport.width > e->viewport.height ? e->viewport.width : e->viewport.height) / 100.0;
    return true;
  default:
    u->status = GTEXT_CSS_E_UNSUPPORTED;
    u->detail = gtext_css_unit_name(unit);
    return false;
  }
}

static bool math_unit_size(void * user, GTEXT_CSS_Unit unit, double * canonical) {
  unit_ctx * u = user;
  if (gtext_css_unit_type(unit) != GTEXT_CSS_UNIT_TYPE_LENGTH) {
    return false;
  }
  unit_ctx tmp = *u;
  tmp.status = GTEXT_CSS_OK;
  bool ok = unit_px(&tmp, unit, canonical);
  if (!ok) {
    u->status = tmp.status;
    u->detail = tmp.detail;
  }
  return ok;
}

/// Resolve a value to a double in the canonical unit of its feature: px, dppx, or the number itself.
static bool value_double(unit_ctx * u, const mq_value * v, int unit_family, double * out) {
  switch ((mq_value_kind)v->kind) {
  case MQV_NUMBER:
    *out = v->a;
    return true;
  case MQV_INFINITE:
    *out = mq_inf();
    return true;
  case MQV_DIMENSION: {
    if (unit_family == MQU_RESOLUTION) {
      double c;
      if (!gtext_css_unit_to_canonical(v->unit, v->a, &c)) {
        return false;
      }
      *out = c;
      return true;
    }
    double per;
    if (!unit_px(u, v->unit, &per)) {
      return false;
    }
    *out = v->a * per;
    return true;
  }
  case MQV_MATH: {
    GTEXT_CSS_Math_Env me;
    memset(&me, 0, sizeof me);
    me.unit_size = math_unit_size;
    me.user = u;
    double r;
    bool resolved = false;
    GTEXT_CSS_Status st = gtext_css_math_resolve(v->math, &me, NULL, &r, &resolved);
    if (st != GTEXT_CSS_OK) {
      u->status = st;
      return false;
    }
    if (!resolved) {
      if (u->status == GTEXT_CSS_OK) {
        u->status = GTEXT_CSS_E_UNSUPPORTED;
        u->detail = "a unit of a calculation";
      }
      return false;
    }
    *out = gtext_css_math_clamp(r, -mq_inf(), mq_inf());
    /* A math function whose result is a ratio's number: also the number. */
    return true;
  }
  case MQV_RATIO:
  case MQV_IDENT:
    return false;
  }
  return false;
}

/* ---- ratios --------------------------------------------------------------- */

/// -1, 0, 1, or 2 (unordered) for n1/d1 against n2/d2 by cross multiplication; a degenerate ratio is read as the extended number it stands for.
static int ratio_cmp(double n1, double d1, double n2, double d2) {
  /* a / b as an extended number: 0 / 0 and inf / inf are NaN (unordered). */
  int k1 = 0, k2 = 0; /* 0 finite positive, 1 +inf, 2 zero, 3 NaN */
  if ((n1 == 0 && d1 == 0) || (mq_isinf(n1) && mq_isinf(d1))) {
    k1 = 3;
  } else if (d1 == 0 || mq_isinf(n1)) {
    k1 = 1;
  } else if (n1 == 0 || mq_isinf(d1)) {
    k1 = 2;
  }
  if ((n2 == 0 && d2 == 0) || (mq_isinf(n2) && mq_isinf(d2))) {
    k2 = 3;
  } else if (d2 == 0 || mq_isinf(n2)) {
    k2 = 1;
  } else if (n2 == 0 || mq_isinf(d2)) {
    k2 = 2;
  }
  if (k1 == 3 || k2 == 3) {
    return 2;
  }
  if (k1 != 0 || k2 != 0) {
    /* rank: zero < finite < infinite */
    int r1 = k1 == 2 ? 0 : (k1 == 0 ? 1 : 2);
    int r2 = k2 == 2 ? 0 : (k2 == 0 ? 1 : 2);
    return r1 < r2 ? -1 : (r1 > r2 ? 1 : 0);
  }
  /* n1 * d2 against n2 * d1, each product kept as mantissa and exponent so that it cannot overflow. */
  int e1a, e1b, e2a, e2b;
  double m1a = gmath_frexp(n1, &e1a), m1b = gmath_frexp(d2, &e1b);
  double m2a = gmath_frexp(n2, &e2a), m2b = gmath_frexp(d1, &e2b);
  double p1 = m1a * m1b, p2 = m2a * m2b;
  long ex1 = (long)e1a + e1b, ex2 = (long)e2a + e2b;
  /* p in [0.25, 1): normalize so that equal products compare equal. */
  int t;
  p1 = gmath_frexp(p1, &t);
  ex1 += t;
  p2 = gmath_frexp(p2, &t);
  ex2 += t;
  if (ex1 != ex2) {
    return ex1 < ex2 ? -1 : 1;
  }
  return p1 < p2 ? -1 : (p1 > p2 ? 1 : 0);
}

static bool cmp_apply(int c, unsigned op) {
  if (c == 2) {
    return false;
  }
  switch (op) {
  case MQOP_LT: return c < 0;
  case MQOP_LE: return c <= 0;
  case MQOP_GT: return c > 0;
  case MQOP_GE: return c >= 0;
  default: return c == 0;
  }
}

static int dcmp(double a, double b) {
  if (mq_isnan(a) || mq_isnan(b)) {
    return 2;
  }
  return a < b ? -1 : (a > b ? 1 : 0);
}

static unsigned flip(unsigned op) {
  switch (op) {
  case MQOP_LT: return MQOP_GT;
  case MQOP_LE: return MQOP_GE;
  case MQOP_GT: return MQOP_LT;
  case MQOP_GE: return MQOP_LE;
  default: return op;
  }
}

/* ---- the features ---------------------------------------------------------- */

/// The current value of a keyword feature as an index into its keywords, or -1 for "matches no keyword".
static int kw_index(const GTEXT_CSS_Media_Environment * e, int row) {
  switch ((css_mq_feature_id)row) {
  case CSS_MQF_ORIENTATION: return e->viewport.height >= e->viewport.width ? 0 : 1;
  case CSS_MQF_SCAN: return (int)e->scan - 1;
  case CSS_MQF_UPDATE: return e->update;
  case CSS_MQF_OVERFLOW_BLOCK: return e->overflow_block;
  case CSS_MQF_OVERFLOW_INLINE: return e->overflow_inline;
  case CSS_MQF_POINTER: return e->pointer;
  case CSS_MQF_HOVER: return e->hover;
  case CSS_MQF_DISPLAY_MODE: return e->display_mode;
  case CSS_MQF_DISPLAY_STATE: return e->display_state;
  case CSS_MQF_RESIZABLE: return e->resizable ? 0 : 1;
  case CSS_MQF_SHAPE: return e->shape;
  case CSS_MQF_ENVIRONMENT_BLENDING: return e->environment_blending;
  case CSS_MQF_NAV_CONTROLS: return e->nav_controls;
  case CSS_MQF_INVERTED_COLORS: return e->inverted_colors;
  case CSS_MQF_SCRIPTING: return e->scripting;
  case CSS_MQF_FORCED_COLORS: return e->forced_colors;
  case CSS_MQF_PREFERS_COLOR_SCHEME: return e->prefers_color_scheme;
  case CSS_MQF_UA_COLOR_SCHEME: return e->ua_color_scheme;
  case CSS_MQF_PREFERS_CONTRAST: return e->prefers_contrast;
  case CSS_MQF_PREFERS_REDUCED_MOTION: return e->prefers_reduced_motion;
  case CSS_MQF_PREFERS_REDUCED_TRANSPARENCY: return e->prefers_reduced_transparency;
  case CSS_MQF_PREFERS_REDUCED_DATA: return e->prefers_reduced_data;
  default: return -1;
  }
}

/// A keyword feature whose value is ordered: each level implies the lower ones. Returns the level (the count of keywords the device matches), or -1 if the
/// feature is not one.
static int kw_level(const GTEXT_CSS_Media_Environment * e, int row) {
  switch ((css_mq_feature_id)row) {
  case CSS_MQF_COLOR_GAMUT: return e->color_gamut;
  case CSS_MQF_VIDEO_COLOR_GAMUT: return e->video_color_gamut;
  case CSS_MQF_DYNAMIC_RANGE: return (int)e->dynamic_range + 1;
  case CSS_MQF_VIDEO_DYNAMIC_RANGE: return (int)e->video_dynamic_range + 1;
  default: return -1;
  }
}

/// The set of keywords the device matches, as a bit mask over the feature's keyword indexes (any-pointer, any-hover), or 0 if the feature is not one.
static bool kw_mask(const GTEXT_CSS_Media_Environment * e, int row, uint32_t * mask) {
  switch ((css_mq_feature_id)row) {
  case CSS_MQF_ANY_POINTER: *mask = e->any_pointer; return true;
  case CSS_MQF_ANY_HOVER: *mask = e->any_hover; return true;
  default: return false;
  }
}

/// Whether the (discrete) feature matches keyword `k` of its list.
static bool kw_matches(const GTEXT_CSS_Media_Environment * e, int row, int k) {
  int lvl = kw_level(e, row);
  if (lvl >= 0) {
    return lvl >= k + 1;
  }
  uint32_t mask;
  if (kw_mask(e, row, &mask)) {
    return (mask >> k) & 1u;
  }
  return kw_index(e, row) == k;
}

/// The boolean context of a keyword feature: true for any value but `none`.
static bool kw_boolean(const GTEXT_CSS_Media_Environment * e, int row) {
  int none = css_mq_find_keyword(row, "none", 4);
  const css_mq_feature_row * r = &css_mq_feature_rows[row];
  for (size_t k = 0; k < r->kw_count; k++) {
    if ((int)k != none && kw_matches(e, row, (int)k)) {
      return true;
    }
  }
  return false;
}

typedef struct {
  double a; ///< The number, or a ratio's numerator.
  double b; ///< A ratio's denominator.
} measure;

/// The feature's value in the environment, for the numeric features.
static void feature_measure(const GTEXT_CSS_Media_Environment * e, int row, measure * m) {
  m->b = 1;
  switch ((css_mq_feature_id)row) {
  case CSS_MQF_WIDTH: m->a = e->viewport.width; break;
  case CSS_MQF_HEIGHT: m->a = e->viewport.height; break;
  case CSS_MQF_DEVICE_WIDTH: m->a = e->device.width; break;
  case CSS_MQF_DEVICE_HEIGHT: m->a = e->device.height; break;
  case CSS_MQF_ASPECT_RATIO: m->a = e->viewport.width; m->b = e->viewport.height; break;
  case CSS_MQF_DEVICE_ASPECT_RATIO: m->a = e->device.width; m->b = e->device.height; break;
  case CSS_MQF_RESOLUTION: m->a = e->resolution_infinite ? mq_inf() : e->resolution; break;
  case CSS_MQF_WEBKIT_DEVICE_PIXEL_RATIO: m->a = e->resolution_infinite ? mq_inf() : e->resolution; break;
  case CSS_MQF_COLOR: m->a = e->color_bits; break;
  case CSS_MQF_COLOR_INDEX: m->a = e->color_index; break;
  case CSS_MQF_MONOCHROME: m->a = e->monochrome_bits; break;
  case CSS_MQF_HORIZONTAL_VIEWPORT_SEGMENTS: m->a = e->horizontal_viewport_segments; break;
  case CSS_MQF_VERTICAL_VIEWPORT_SEGMENTS: m->a = e->vertical_viewport_segments; break;
  case CSS_MQF_GRID: m->a = e->grid ? 1 : 0; break;
  case CSS_MQF_WEBKIT_TRANSFORM_3D: m->a = e->transform_3d ? 1 : 0; break;
  default: m->a = 0; break;
  }
}

typedef struct {
  GTEXT_CSS_Status status;
  const char * detail;
  GTEXT_CSS_Span where;
} efail;

static GTEXT_CSS_Tristate tri_of(bool b) {
  return b ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
}

GTEXT_CSS_Tristate css_mq_eval_feature(const GTEXT_CSS_Media_Environment * e, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status,
    const char ** detail) {
  int row = n->feature;
  const css_mq_feature_row * fr = &css_mq_feature_rows[row];
  if (!get_bit(e, row) || fr->status != MQS_IMPLEMENTED) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  unit_ctx u = {e, NULL, GTEXT_CSS_OK};
  /* A keyword feature, or an mq-boolean. */
  if (fr->unit == MQU_KEYWORD) {
    if (n->form == MQFORM_BOOLEAN) {
      return tri_of(kw_boolean(e, row));
    }
    const mq_value * v = &in->values[n->v1];
    return tri_of(kw_matches(e, row, v->keyword));
  }
  measure m;
  feature_measure(e, row, &m);
  if (fr->unit == MQU_BOOLEAN) {
    if (n->form == MQFORM_BOOLEAN) {
      return tri_of(m.a != 0);
    }
    double want;
    if (!value_double(&u, &in->values[n->v1], MQU_NUMBER, &want)) {
      if (u.status != GTEXT_CSS_OK) {
        *status = u.status;
        *detail = u.detail;
      }
      return GTEXT_CSS_TRI_UNKNOWN;
    }
    return tri_of(m.a == want);
  }
  if (n->form == MQFORM_BOOLEAN) {
    /* True for any value but 0. A ratio is 0 when its numerator is. */
    return tri_of(m.a != 0 && !mq_isnan(m.a));
  }
  /* Numeric: the value to compare against. */
  double vals[2] = {0, 0};
  measure rv[2] = {{0, 1}, {0, 1}};
  uint32_t vi[2] = {n->v1, n->v2};
  for (int i = 0; i < 2; i++) {
    if (vi[i] == MQ_NONE) {
      continue;
    }
    const mq_value * v = &in->values[vi[i]];
    if (fr->unit == MQU_RATIO) {
      rv[i].a = v->a;
      rv[i].b = v->b;
      mq_value tmp;
      memset(&tmp, 0, sizeof tmp);
      tmp.kind = MQV_MATH;
      GTEXT_CSS_Math * ms[2] = {v->math, v->math2};
      double * dst[2] = {&rv[i].a, &rv[i].b};
      for (int k = 0; k < 2; k++) {
        if (!ms[k]) {
          continue;
        }
        tmp.math = ms[k];
        if (!value_double(&u, &tmp, MQU_NUMBER, dst[k])) {
          if (u.status != GTEXT_CSS_OK) {
            *status = u.status;
            *detail = u.detail;
          }
          return GTEXT_CSS_TRI_UNKNOWN;
        }
        if (*dst[k] < 0) {
          return GTEXT_CSS_TRI_UNKNOWN;
        }
      }
    } else if (!value_double(&u, v, fr->unit, &vals[i])) {
      if (u.status != GTEXT_CSS_OK) {
        *status = u.status;
        *detail = u.detail;
      }
      return GTEXT_CSS_TRI_UNKNOWN;
    }
  }
  /* A comparison of the feature against the i-th value: the sign of (feature - value). */
#define CMPV(i) (fr->unit == MQU_RATIO ? ratio_cmp(m.a, m.b, rv[i].a, rv[i].b) : dcmp(m.a, vals[i]))
  if (n->form == MQFORM_PLAIN) {
    unsigned op = n->prefix == MQP_MIN ? MQOP_GE : (n->prefix == MQP_MAX ? MQOP_LE : MQOP_EQ);
    return tri_of(cmp_apply(CMPV(0), op));
  }
  /* The range forms. */
  if (n->pos == MQPOS_LEFT) {
    return tri_of(cmp_apply(CMPV(0), n->op1));
  }
  if (n->pos == MQPOS_RIGHT) {
    return tri_of(cmp_apply(CMPV(0), flip(n->op1)));
  }
  return tri_of(cmp_apply(CMPV(0), flip(n->op1)) && cmp_apply(CMPV(1), n->op2));
#undef CMPV
}

/* ---- the machine ------------------------------------------------------------ */

void gtext_css_condition_call_init(GTEXT_CSS_Condition_Call * call) {
  if (call) {
    memset(call, 0, sizeof *call);
  }
}

void gtext_css_condition_call_free(GTEXT_CSS_Condition_Call * call) {
  if (!call) {
    return;
  }
  if (call->internal) {
    GTEXT_Allocator a = call->internal->alloc;
    gtext_allocator_free(&a, call->internal->memo);
    gtext_allocator_free(&a, call->internal->stack);
    gtext_allocator_free(&a, call->internal);
  }
  memset(call, 0, sizeof *call);
}

static GTEXT_CSS_Tristate type_match(const GTEXT_CSS_Media_Environment * e, const mq_node * q) {
  if (!(q->flags & MQF_HAS_TYPE)) {
    return GTEXT_CSS_TRI_TRUE;
  }
  switch ((mq_type_id)q->type_id) {
  case MQT_ID_ALL: return GTEXT_CSS_TRI_TRUE;
  case MQT_ID_SCREEN: return tri_of(e->medium == GTEXT_CSS_MEDIUM_SCREEN);
  case MQT_ID_PRINT: return tri_of(e->medium == GTEXT_CSS_MEDIUM_PRINT);
  default: return GTEXT_CSS_TRI_FALSE;
  }
}

/// Evaluate query `qi` of `list` (the root). Returns the three-valued answer in *out.
GTEXT_CSS_Status css_conditions_eval_query(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const struct GTEXT_CSS_Media_List_Internal * list, uint32_t qnode,
    GTEXT_CSS_Tristate * out) {
  struct GTEXT_CSS_Condition_Call_Internal * ci = call->internal;
  const GTEXT_CSS_Media_Environment * env = &c->env;
  size_t sp = 0;
  GTEXT_CSS_Tristate ret = GTEXT_CSS_TRI_UNKNOWN;
  size_t work = 0;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
#define PUSH(IN, NODE, DEF)                                                                                      \
  do {                                                                                                           \
    if (sp == ci->cap) {                                                                                         \
      size_t nc = ci->cap ? ci->cap * 2 : 64;                                                                    \
      eframe * ns = gtext_allocator_realloc(&ci->alloc, ci->stack, nc * sizeof(eframe));                         \
      if (!ns) {                                                                                                 \
        status = GTEXT_CSS_E_OOM;                                                                                \
        goto done;                                                                                               \
      }                                                                                                          \
      ci->stack = ns;                                                                                            \
      ci->cap = nc;                                                                                              \
    }                                                                                                            \
    ci->stack[sp].in = (IN);                                                                                     \
    ci->stack[sp].node = (NODE);                                                                                 \
    ci->stack[sp].idx = 0;                                                                                       \
    ci->stack[sp].def = (DEF);                                                                                   \
    ci->stack[sp].acc = 0;                                                                                       \
    sp++;                                                                                                        \
  } while (0)
  PUSH(list, qnode, -1);
  while (sp) {
    eframe * f = &ci->stack[sp - 1];
    if (++work > c->max_work) {
      status = GTEXT_CSS_E_LIMIT_WORK;
      goto done;
    }
    call->node_visits++;
    if (f->def >= 0) {
      /* A custom media definition: true if some query of its list is true. */
      const struct GTEXT_CSS_Media_List_Internal * din = f->in;
      if (f->idx > 0) {
        if (ret == GTEXT_CSS_TRI_TRUE) {
          f->acc = 1;
          f->idx = (uint32_t)din->root_count; /* done */
        }
      }
      if (f->idx >= din->root_count) {
        ci->memo[f->def] = f->acc ? 1 : 2;
        call->defs_evaluated++;
        ret = tri_of(f->acc != 0);
        sp--;
        continue;
      }
      uint32_t qn = din->roots[f->idx++];
      PUSH(din, qn, -1);
      continue;
    }
    const mq_node * n = &f->in->nodes[f->node];
    switch ((mq_node_kind)n->kind) {
    case MQN_QUERY: {
      if (f->idx == 0) {
        if (n->flags & MQF_REPLACED) {
          ret = GTEXT_CSS_TRI_FALSE;
          sp--;
          break;
        }
        GTEXT_CSS_Tristate t = type_match(env, n);
        if (n->kid_count == 0 || t == GTEXT_CSS_TRI_FALSE) {
          ret = (n->flags & MQF_NEGATED) ? css_tri_not(t) : t;
          sp--;
          break;
        }
        f->idx = 1;
        PUSH(f->in, f->in->kids[n->kid_first], -1);
        break;
      }
      /* ret is the condition; the type was true. */
      if (n->flags & MQF_NEGATED) {
        ret = css_tri_not(ret);
      }
      sp--;
      break;
    }
    case MQN_NOT:
      if (f->idx == 0) {
        f->idx = 1;
        PUSH(f->in, f->in->kids[n->kid_first], -1);
        break;
      }
      ret = css_tri_not(ret);
      sp--;
      break;
    case MQN_PAREN:
      if (f->idx == 0) {
        f->idx = 1;
        PUSH(f->in, f->in->kids[n->kid_first], -1);
        break;
      }
      sp--;
      break;
    case MQN_AND:
    case MQN_OR: {
      bool is_and = n->kind == MQN_AND;
      if (f->idx == 0) {
        f->acc = is_and ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
      } else {
        f->acc = (uint8_t)(is_and ? css_tri_and((GTEXT_CSS_Tristate)f->acc, ret) : css_tri_or((GTEXT_CSS_Tristate)f->acc, ret));
        /* False dominates `and`, true dominates `or`: the rest cannot change the answer. */
        if (f->acc == (is_and ? GTEXT_CSS_TRI_FALSE : GTEXT_CSS_TRI_TRUE)) {
          f->idx = n->kid_count;
        }
      }
      if (f->idx >= n->kid_count) {
        ret = (GTEXT_CSS_Tristate)f->acc;
        sp--;
        break;
      }
      uint32_t k = f->in->kids[n->kid_first + f->idx];
      f->idx++;
      PUSH(f->in, k, -1);
      break;
    }
    case MQN_FEATURE: {
      const char * detail = NULL;
      ret = css_mq_eval_feature(env, f->in, n, &status, &detail);
      if (status != GTEXT_CSS_OK) {
        call->detail = detail;
        call->where = n->span;
        goto done;
      }
      sp--;
      break;
    }
    case MQN_ENCLOSED: {
      ret = GTEXT_CSS_TRI_UNKNOWN;
      if (n->name && env->extension_feature) {
        GTEXT_CSS_String nm = {n->name, n->name_length};
        GTEXT_CSS_String vl = {n->value, n->value_length};
        ret = env->extension_feature(env->user, &nm, n->value ? &vl : NULL);
      }
      sp--;
      break;
    }
    case MQN_CUSTOM: {
      ret = GTEXT_CSS_TRI_UNKNOWN;
      int w = c->custom_media ? css_defs_find(c->defs, n->text, n->text_length) : -1;
      if (w < 0 || css_defs_is_cyclic(c->defs, (size_t)w)) {
        sp--;
        break;
      }
      if (ci->memo[w]) {
        ret = tri_of(ci->memo[w] == 1);
        call->memo_hits++;
        sp--;
        break;
      }
      int kind = css_defs_kind(c->defs, (size_t)w);
      if (kind != CSS_DEF_LIST) {
        ci->memo[w] = kind == CSS_DEF_TRUE ? 1 : 2;
        ret = tri_of(kind == CSS_DEF_TRUE);
        call->defs_evaluated++;
        sp--;
        break;
      }
      /* The reference is finished by the definition: replace this frame with it. */
      sp--;
      PUSH(css_defs_list(c->defs, (size_t)w), MQ_NONE, w);
      break;
    }
    }
  }
  *out = ret;
done:
  call->work = work;
  return status;
#undef PUSH
}
