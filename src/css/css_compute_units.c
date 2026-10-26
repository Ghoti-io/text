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
 * The units of a computation: what a relative length is in px for the property being computed (Values 4, 6.1 and 6.1.2: font-relative lengths
 * against the font-size of the element, or of its parent for `font-size` itself; viewport lengths against the environment; container lengths by
 * the environment's callback), and the resolution of a relative URL (RFC 3986, 5.2).
 */

#include <string.h>

#include "css_compute_internal.h"

/* ---- the font of the element ----------------------------------------------------------------------- */

static double font_px_of(const css_state * st, const GTEXT_CSS_Computed_Style * style) {
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  if (!style || !cc->pid[CSS_P_FONT_SIZE]) {
    return st->env->default_font_size;
  }
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(style->vals[cc->pid[CSS_P_FONT_SIZE] - 1]);
  if (v && v->kind == GTEXT_CSS_VAL_DIMENSION && v->unit == GTEXT_CSS_UNIT_PX) {
    return v->number;
  }
  return st->env->default_font_size;
}

double css_st_parent_font_px(css_state * st) {
  return font_px_of(st, st->parent);
}

double css_st_em(css_state * st) {
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  if (st->current == cc->pid[CSS_P_FONT_SIZE]) {
    return css_st_parent_font_px(st);
  }
  return font_px_of(st, st->style);
}

void css_st_font_query(css_state * st, bool root, GTEXT_CSS_Font_Query * q) {
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  if (root) {
    const css_root_info * r = &st->style->root;
    memset(q, 0, sizeof *q);
    q->element = NULL;
    if (r->valid) {
      q->family = gtext_css_value_set_root(r->family);
      q->size_px = r->font_px;
      q->weight = r->weight;
      q->width_percent = r->width_percent;
      q->italic = r->italic;
      q->oblique_degrees = r->oblique_degrees;
      q->vertical = r->vertical;
    }
    else {
      /* the root is being computed: its font is not known yet, and the initial one has the default size */
      q->size_px = st->env->default_font_size;
      q->weight = 400.0;
      q->width_percent = 100.0;
      q->vertical = st->style->flow.writing_mode != GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB;
    }
    return;
  }
  if (st->current == cc->pid[CSS_P_FONT_SIZE] && st->parent) {
    css_st_font_query_of(st->parent, css_st_parent_font_px(st), q);
    q->element = st->element;
    return;
  }
  css_st_font_query_of(st->style, font_px_of(st, st->style), q);
  q->element = st->element;
}

/// The line height of a style in px; false (and the failure set) when it is `normal` and the environment cannot say.
static bool line_height_px(css_state * st, const GTEXT_CSS_Computed_Style * style, const GTEXT_CSS_Value * lh, double font_px, bool root,
    const char * unit_name, double * px) {
  if (lh && lh->kind == GTEXT_CSS_VAL_NUMBER) {
    *px = lh->number * font_px;
    return true;
  }
  if (lh && lh->kind == GTEXT_CSS_VAL_DIMENSION && lh->unit == GTEXT_CSS_UNIT_PX) {
    *px = lh->number;
    return true;
  }
  /* normal (or a value that has no length yet): the environment's metric */
  if (!st->env->normal_line_height) {
    css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, unit_name, "the environment has no normal_line_height callback");
    return false;
  }
  GTEXT_CSS_Font_Query q;
  if (root) {
    css_st_font_query(st, true, &q);
  }
  else {
    css_st_font_query_of(style, font_px, &q);
    q.element = st->element;
  }
  if (!st->env->normal_line_height(st->env->user, &q, px)) {
    css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, unit_name, "the environment gave no normal line height for the font");
    return false;
  }
  return true;
}

static bool metric_px(css_state * st, GTEXT_CSS_Unit unit, bool root, double * px) {
  GTEXT_CSS_Font_Metric metric = GTEXT_CSS_METRIC_EX;
  switch (unit) {
  case GTEXT_CSS_UNIT_EX: case GTEXT_CSS_UNIT_REX: metric = GTEXT_CSS_METRIC_EX; break;
  case GTEXT_CSS_UNIT_CH: case GTEXT_CSS_UNIT_RCH: metric = GTEXT_CSS_METRIC_CH; break;
  case GTEXT_CSS_UNIT_CAP: case GTEXT_CSS_UNIT_RCAP: metric = GTEXT_CSS_METRIC_CAP; break;
  default: metric = GTEXT_CSS_METRIC_IC; break;
  }
  const char * name = gtext_css_unit_name(unit);
  if (!st->env->font_metric) {
    css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, name, "the environment has no font_metric callback");
    return false;
  }
  GTEXT_CSS_Font_Query q;
  css_st_font_query(st, root, &q);
  double value = 0.0;
  if (!st->env->font_metric(st->env->user, &q, metric, &value)) {
    css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, name, "the environment has no such metric for the font");
    return false;
  }
  /* the callback gives the metric at the query's size; the unit is that metric (a length, in px) */
  *px = value;
  return true;
}

/* ---- one unit in px --------------------------------------------------------------------------------- */

static double viewport_part(const GTEXT_CSS_Size * s, int axis) {
  return (axis == 0 ? s->width : s->height) / 100.0;
}

bool css_st_unit_px(css_state * st, GTEXT_CSS_Unit unit, double * px) {
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  const GTEXT_CSS_Environment * env = st->env;
  const char * name = gtext_css_unit_name(unit);
  switch (unit) {
  case GTEXT_CSS_UNIT_EM:
    *px = css_st_em(st);
    return true;
  case GTEXT_CSS_UNIT_REM:
    *px = st->style->root.valid ? st->style->root.font_px : (st->parent ? st->parent->root.font_px : env->default_font_size);
    return true;
  case GTEXT_CSS_UNIT_EX: case GTEXT_CSS_UNIT_CH: case GTEXT_CSS_UNIT_CAP: case GTEXT_CSS_UNIT_IC:
    return metric_px(st, unit, false, px);
  case GTEXT_CSS_UNIT_REX: case GTEXT_CSS_UNIT_RCH: case GTEXT_CSS_UNIT_RCAP: case GTEXT_CSS_UNIT_RIC:
    return metric_px(st, unit, true, px);
  case GTEXT_CSS_UNIT_LH: {
    /* the line height of the element, or of its parent in line-height itself */
    if (st->current == cc->pid[CSS_P_LINE_HEIGHT]) {
      if (st->parent) {
        const double fp = css_st_parent_font_px(st);
        return line_height_px(st, st->parent, gtext_css_value_set_root(st->parent->vals[cc->pid[CSS_P_LINE_HEIGHT] - 1]), fp, false, name, px);
      }
      return line_height_px(st, st->style, NULL, env->default_font_size, false, name, px);
    }
    const double f = font_px_of(st, st->style);
    return line_height_px(st, st->style, css_st_root(st, cc->pid[CSS_P_LINE_HEIGHT]), f, false, name, px);
  }
  case GTEXT_CSS_UNIT_RLH: {
    const css_root_info * r = st->style->root.valid ? &st->style->root : (st->parent ? &st->parent->root : &st->style->root);
    return line_height_px(st, st->style, gtext_css_value_set_root(r->line_height), r->valid ? r->font_px : env->default_font_size, true, name, px);
  }
  case GTEXT_CSS_UNIT_VW: case GTEXT_CSS_UNIT_LVW: *px = viewport_part(&env->large_viewport, 0); return true;
  case GTEXT_CSS_UNIT_SVW: *px = viewport_part(&env->small_viewport, 0); return true;
  case GTEXT_CSS_UNIT_DVW: *px = viewport_part(&env->dynamic_viewport, 0); return true;
  case GTEXT_CSS_UNIT_VH: case GTEXT_CSS_UNIT_LVH: *px = viewport_part(&env->large_viewport, 1); return true;
  case GTEXT_CSS_UNIT_SVH: *px = viewport_part(&env->small_viewport, 1); return true;
  case GTEXT_CSS_UNIT_DVH: *px = viewport_part(&env->dynamic_viewport, 1); return true;
  default:
    break;
  }
  /* the inline and block axes of the viewport are those of the root element's writing mode; min and max are of both axes */
  const bool root_vertical = st->style->root.valid ? st->style->root.vertical
      : (st->parent && st->parent->root.valid ? st->parent->root.vertical : st->style->flow.writing_mode != GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB);
  const GTEXT_CSS_Size * vp = NULL;
  int kind = 0; /* 0 inline, 1 block, 2 min, 3 max */
  switch (unit) {
  case GTEXT_CSS_UNIT_VI: case GTEXT_CSS_UNIT_LVI: vp = &env->large_viewport; kind = 0; break;
  case GTEXT_CSS_UNIT_SVI: vp = &env->small_viewport; kind = 0; break;
  case GTEXT_CSS_UNIT_DVI: vp = &env->dynamic_viewport; kind = 0; break;
  case GTEXT_CSS_UNIT_VB: case GTEXT_CSS_UNIT_LVB: vp = &env->large_viewport; kind = 1; break;
  case GTEXT_CSS_UNIT_SVB: vp = &env->small_viewport; kind = 1; break;
  case GTEXT_CSS_UNIT_DVB: vp = &env->dynamic_viewport; kind = 1; break;
  case GTEXT_CSS_UNIT_VMIN: case GTEXT_CSS_UNIT_LVMIN: vp = &env->large_viewport; kind = 2; break;
  case GTEXT_CSS_UNIT_SVMIN: vp = &env->small_viewport; kind = 2; break;
  case GTEXT_CSS_UNIT_DVMIN: vp = &env->dynamic_viewport; kind = 2; break;
  case GTEXT_CSS_UNIT_VMAX: case GTEXT_CSS_UNIT_LVMAX: vp = &env->large_viewport; kind = 3; break;
  case GTEXT_CSS_UNIT_SVMAX: vp = &env->small_viewport; kind = 3; break;
  case GTEXT_CSS_UNIT_DVMAX: vp = &env->dynamic_viewport; kind = 3; break;
  default: break;
  }
  if (vp) {
    const double w = vp->width / 100.0, h = vp->height / 100.0;
    switch (kind) {
    case 0: *px = root_vertical ? h : w; break;
    case 1: *px = root_vertical ? w : h; break;
    case 2: *px = w < h ? w : h; break;
    default: *px = w < h ? h : w; break;
    }
    return true;
  }
  /* container query lengths */
  GTEXT_CSS_Container_Axis axes[2];
  size_t naxes = 1;
  bool take_min = false, take_max = false;
  switch (unit) {
  case GTEXT_CSS_UNIT_CQW: axes[0] = GTEXT_CSS_CONTAINER_WIDTH; break;
  case GTEXT_CSS_UNIT_CQH: axes[0] = GTEXT_CSS_CONTAINER_HEIGHT; break;
  case GTEXT_CSS_UNIT_CQI: axes[0] = GTEXT_CSS_CONTAINER_INLINE; break;
  case GTEXT_CSS_UNIT_CQB: axes[0] = GTEXT_CSS_CONTAINER_BLOCK; break;
  case GTEXT_CSS_UNIT_CQMIN: axes[0] = GTEXT_CSS_CONTAINER_INLINE; axes[1] = GTEXT_CSS_CONTAINER_BLOCK; naxes = 2; take_min = true; break;
  case GTEXT_CSS_UNIT_CQMAX: axes[0] = GTEXT_CSS_CONTAINER_INLINE; axes[1] = GTEXT_CSS_CONTAINER_BLOCK; naxes = 2; take_max = true; break;
  default:
    css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, name ? name : "unit", "this unit is not a length that has a pixel size");
    return false;
  }
  if (!env->container_size) {
    css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, name, "the environment has no container_size callback");
    return false;
  }
  double v[2] = {0, 0};
  for (size_t i = 0; i < naxes; i++) {
    double size = 0;
    if (!env->container_size(env->user, st->element, axes[i], &size)) {
      /* "If no eligible query container is available, then use the small viewport size for that axis" (Values 4, 6.1.3) */
      const bool vertical_axis = axes[i] == GTEXT_CSS_CONTAINER_HEIGHT ||
          (axes[i] == GTEXT_CSS_CONTAINER_INLINE && root_vertical) || (axes[i] == GTEXT_CSS_CONTAINER_BLOCK && !root_vertical);
      size = vertical_axis ? env->small_viewport.height : env->small_viewport.width;
    }
    v[i] = size / 100.0;
  }
  *px = naxes == 1 ? v[0] : take_min ? (v[0] < v[1] ? v[0] : v[1]) : take_max ? (v[0] < v[1] ? v[1] : v[0]) : v[0];
  return true;
}

/* ---- URLs --------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
  bool oom;
} ubuf;

static bool ub_put(ubuf * b, const char * s, size_t n) {
  if (b->oom) {
    return false;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 128;
    while (cap < b->len + n + 1) {
      if (cap > SIZE_MAX / 2) {
        b->oom = true;
        return false;
      }
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!grown) {
      b->oom = true;
      return false;
    }
    b->data = grown;
    b->cap = cap;
  }
  if (n) {
    memcpy(b->data + b->len, s, n);
  }
  b->len += n;
  b->data[b->len] = '\0';
  return true;
}

static bool scheme_end(const char * s, size_t n, size_t * end) {
  if (n == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
    return false;
  }
  for (size_t i = 1; i < n; i++) {
    const char c = s[i];
    if (c == ':') {
      *end = i;
      return true;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
      return false;
    }
  }
  return false;
}

/// RFC 3986, 5.2.4: the path with `.` and `..` segments removed, appended to `out` (iterative: the output is its own stack of segments).
static void remove_dots(ubuf * out, const char * p, size_t n) {
  const size_t base = out->len;
  size_t i = 0;
  while (i < n && !out->oom) {
    if (n - i >= 3 && memcmp(p + i, "../", 3) == 0) {
      i += 3;
    }
    else if (n - i >= 2 && memcmp(p + i, "./", 2) == 0) {
      i += 2;
    }
    else if (n - i >= 3 && memcmp(p + i, "/./", 3) == 0) {
      i += 2;
    }
    else if (n - i == 2 && memcmp(p + i, "/.", 2) == 0) {
      ub_put(out, "/", 1);
      i += 2;
    }
    else if ((n - i >= 4 && memcmp(p + i, "/../", 4) == 0) || (n - i == 3 && memcmp(p + i, "/..", 3) == 0)) {
      size_t cut = out->len;
      while (cut > base && out->data[cut - 1] != '/') {
        cut--;
      }
      if (cut > base) {
        cut--; /* the slash that began the last segment goes with it */
      }
      out->len = cut;
      if (out->data) {
        out->data[out->len] = '\0';
      }
      if (n - i == 3) {
        ub_put(out, "/", 1);
        i += 3;
      }
      else {
        i += 3; /* the "/.." is consumed; the "/" that follows begins the next segment */
      }
    }
    else if ((n - i == 1 && p[i] == '.') || (n - i == 2 && memcmp(p + i, "..", 2) == 0)) {
      i = n;
    }
    else {
      size_t j = i;
      if (p[j] == '/') {
        j++;
      }
      while (j < n && p[j] != '/') {
        j++;
      }
      ub_put(out, p + i, j - i);
      i = j;
    }
  }
}

int css_resolve_url(const GTEXT_Allocator * alloc, const char * base, size_t base_len, const char * ref, size_t ref_len, char ** out,
    size_t * out_len) {
  size_t e;
  *out = NULL;
  if (ref_len == 0 || ref[0] == '#') {
    return 0; /* an empty or fragment-only reference is the document itself: not resolved (Values 4, 4.5) */
  }
  if (scheme_end(ref, ref_len, &e)) {
    return 0; /* already absolute */
  }
  if (!base || !scheme_end(base, base_len, &e)) {
    return 0;
  }
  const size_t scheme_len = e;
  size_t i = scheme_len + 1;
  size_t auth_start = i, auth_end = i;
  bool has_auth = false;
  if (i + 1 < base_len && base[i] == '/' && base[i + 1] == '/') {
    has_auth = true;
    auth_start = i + 2;
    auth_end = auth_start;
    while (auth_end < base_len && base[auth_end] != '/' && base[auth_end] != '?' && base[auth_end] != '#') {
      auth_end++;
    }
    i = auth_end;
  }
  const size_t path_start = i;
  size_t path_end = i;
  while (path_end < base_len && base[path_end] != '?' && base[path_end] != '#') {
    path_end++;
  }
  size_t q_end = path_end;
  if (q_end < base_len && base[q_end] == '?') {
    while (q_end < base_len && base[q_end] != '#') {
      q_end++;
    }
  }
  size_t rp_end = 0;
  while (rp_end < ref_len && ref[rp_end] != '?' && ref[rp_end] != '#') {
    rp_end++;
  }
  const char * tail = ref + rp_end;
  const size_t tail_len = ref_len - rp_end;
  ubuf b;
  memset(&b, 0, sizeof b);
  b.alloc = *alloc;
  ub_put(&b, base, scheme_len + 1);
  if (ref_len >= 2 && ref[0] == '/' && ref[1] == '/') {
    ub_put(&b, ref, ref_len);
  }
  else {
    if (has_auth) {
      ub_put(&b, "//", 2);
      ub_put(&b, base + auth_start, auth_end - auth_start);
    }
    if (rp_end == 0) {
      ub_put(&b, base + path_start, path_end - path_start);
      if (tail[0] != '?' && q_end > path_end) {
        ub_put(&b, base + path_end, q_end - path_end);
      }
    }
    else if (ref[0] == '/') {
      remove_dots(&b, ref, rp_end);
    }
    else {
      ubuf m;
      memset(&m, 0, sizeof m);
      m.alloc = *alloc;
      if (has_auth && path_end == path_start) {
        ub_put(&m, "/", 1);
      }
      else {
        size_t cut = path_end;
        while (cut > path_start && base[cut - 1] != '/') {
          cut--;
        }
        ub_put(&m, base + path_start, cut - path_start);
      }
      ub_put(&m, ref, rp_end);
      if (!m.oom) {
        remove_dots(&b, m.data, m.len);
      }
      else {
        b.oom = true;
      }
      gtext_allocator_free(&m.alloc, m.data);
    }
    ub_put(&b, tail, tail_len);
  }
  if (b.oom) {
    gtext_allocator_free(&b.alloc, b.data);
    return -1;
  }
  *out = b.data;
  *out_len = b.len;
  return 1;
}
