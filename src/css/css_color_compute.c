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
 * Computing `color-mix()`, relative colors, `alpha()`, `contrast-color()` and calculations in color channels (css_color_compute.h).
 *
 * References (Editor's Drafts served 2026-10-30): Color 5, 3.3 "Calculating the Result of color-mix" (the percentages are normalized with the forced
 * flag, the colors are mixed pair by pair in order, and the alpha is multiplied by 1 minus the leftover), 4.2 "Relative Color Syntax" (the keywords are numbers
 * of the origin converted to the function's own space; `none` is 0 in a calculation), 10 "Resolving Color Values", 11 "Serialization"; Color 4, 13
 * "Color Interpolation" (analogous components, premultiplication, the four hue methods), 15 "Resolving Color Values".
 *
 * The tree of a color is walked with an explicit stack of frames and a stack of values, so that a color nested to the limit of the parser is a loop and
 * not a recursion. Every conversion is IEEE double arithmetic and the `math` library's functions (css_color_space.h).
 */

#include <float.h>
#include <stdint.h>
#include <string.h>

#include <ghoti.io/math/math.h>

#include "css_color_compute.h"
#include "css_color_internal.h"
#include "css_internal.h"

typedef GTEXT_CSS_Color color_t;
typedef GTEXT_CSS_Color_Component comp_t;

/* ---- the stacks ----------------------------------------------------------------------------------------------- */

typedef struct {
  const color_t * c;
  size_t next;   ///< The next kid to evaluate.
  size_t kids;   ///< How many kids the node has.
  size_t base;   ///< The height of the value stack when the node began.
} frame;

typedef struct {
  css_color_ctx * ctx;
  frame * fr;
  size_t nfr, capfr;
  css_cc_value * vals;
  size_t nval, capval;
  GTEXT_CSS_Status status;
  bool defer;   ///< Something cannot be computed here: the whole color stays as written.
} walk;

static bool grow(walk * w, void ** p, size_t * cap, size_t need, size_t elem) {
  if (need <= *cap) {
    return true;
  }
  size_t c = *cap ? *cap * 2 : 16;
  while (c < need) {
    c *= 2;
  }
  GTEXT_Allocator a = *w->ctx->alloc;
  void * g = gtext_allocator_realloc(&a, *p, c * elem);
  if (!g) {
    w->status = GTEXT_CSS_E_OOM;
    return false;
  }
  *p = g;
  *cap = c;
  return true;
}

static bool push_frame(walk * w, const color_t * c, size_t kids) {
  if (!grow(w, (void **)&w->fr, &w->capfr, w->nfr + 1, sizeof *w->fr)) {
    return false;
  }
  w->fr[w->nfr++] = (frame){c, 0, kids, w->nval};
  return true;
}

static bool push_value(walk * w, const css_cc_value * v) {
  if (!grow(w, (void **)&w->vals, &w->capval, w->nval + 1, sizeof *w->vals)) {
    return false;
  }
  w->vals[w->nval++] = *v;
  return true;
}

/* ---- numbers ------------------------------------------------------------------------------------------------------ */

static double normalize_hue(double h) {
  if (h != h) {
    return 0.0;
  }
  if (h > DBL_MAX || h < -DBL_MAX) {
    return 0.0;
  }
  double r = gmath_fmod(h, 360.0);
  if (r < 0.0) {
    r += 360.0;
  }
  return r >= 360.0 || r == 0.0 ? 0.0 : r;
}

static double clamp01(double a) {
  return a != a ? 0.0 : a < 0.0 ? 0.0 : a > 1.0 ? 1.0 : a;
}

/* ---- evaluating the channels of a function ------------------------------------------------------------------------ */

/// The channel keywords of a relative color: the origin converted to the function's space, in the units of the function's channels.
typedef struct {
  const css_color_fninfo * info;
  const css_cs * origin;   ///< NULL: no keywords.
} symenv;

typedef struct {
  css_color_ctx * ctx;
  const symenv * se;
} menv;

static bool env_unit(void * user, GTEXT_CSS_Unit unit, double * canonical) {
  menv * m = user;
  return m->ctx->unit_size ? m->ctx->unit_size(m->ctx->user, unit, canonical) : false;
}

static bool env_tree_count(void * user, bool index, double * value) {
  menv * m = user;
  return m->ctx->tree_count ? m->ctx->tree_count(m->ctx->user, index, value) : false;
}

static bool symbol_index(const symenv * se, const char * name, size_t * idx) {
  if (!se || !se->origin) {
    return false;
  }
  for (size_t i = 0; i < 4; i++) {
    if (se->info->sym[i] && strcmp(name, se->info->sym[i]) == 0) {
      *idx = i;
      return true;
    }
  }
  return false;
}

/// The value of a channel keyword (`none` is 0 inside a calculation, Color 5, 4.2).
static bool symbol_value(const symenv * se, size_t i, double * v, bool * none) {
  const css_cs * o = se->origin;
  if (i == 3) {
    *none = o->none[3];
    *v = o->none[3] ? 0.0 : o->alpha;
    return true;
  }
  *none = o->none[i];
  *v = o->none[i] ? 0.0 : o->c[i] * se->info->scale[i];
  return true;
}

static bool env_symbol(void * user, const char * name, double * value) {
  menv * m = user;
  size_t i;
  bool none;
  if (!symbol_index(m->se, name, &i)) {
    return false;
  }
  return symbol_value(m->se, i, value, &none);
}

/// Evaluate one component to a number in the units of the channel (`ref` is what 100% is; the hue and the alpha have their own). `*none` is set for a missing
/// component. False: the component needs something this environment cannot give (the color stays as written).
static GTEXT_CSS_Status comp_value(walk * w, const comp_t * k, double ref, const symenv * se, double * v, bool * none, bool * ok) {
  css_color_ctx * ctx = w->ctx;
  ctx->work++;
  *none = false;
  *ok = true;
  switch (k->kind) {
  case GTEXT_CSS_COMP_NUMBER:
  case GTEXT_CSS_COMP_ANGLE:
    *v = k->value;
    return GTEXT_CSS_OK;
  case GTEXT_CSS_COMP_PERCENT:
    *v = k->value / 100.0 * ref;
    return GTEXT_CSS_OK;
  case GTEXT_CSS_COMP_NONE:
    *none = true;
    *v = 0.0;
    return GTEXT_CSS_OK;
  case GTEXT_CSS_COMP_SYMBOL: {
    size_t i;
    char name[16];
    if (!se || k->text.length >= sizeof name) {
      *ok = false;
      return GTEXT_CSS_OK;
    }
    memcpy(name, k->text.data, k->text.length);
    name[k->text.length] = '\0';
    for (size_t j = 0; name[j]; j++) {
      if (name[j] >= 'A' && name[j] <= 'Z') {
        name[j] = (char)(name[j] + 32);
      }
    }
    if (!symbol_index(se, name, &i)) {
      *ok = false;
      return GTEXT_CSS_OK;
    }
    symbol_value(se, i, v, none);
    return GTEXT_CSS_OK;
  }
  case GTEXT_CSS_COMP_MATH: {
    menv m = {ctx, se};
    GTEXT_CSS_Math_Env me;
    memset(&me, 0, sizeof me);
    me.unit_size = env_unit;
    me.symbol = env_symbol;
    me.tree_count = env_tree_count;
    me.user = &m;
    me.has_percent_basis = true;
    me.percent_basis = ref;
    bool resolved = false;
    double r = 0.0;
    GTEXT_Allocator a = *ctx->alloc;
    const GTEXT_CSS_Status s = gtext_css_math_resolve(k->math, &me, &a, &r, &resolved);
    if (s != GTEXT_CSS_OK) {
      w->status = s;
      return s;
    }
    if (!resolved) {
      *ok = false;
      return GTEXT_CSS_OK;
    }
    *v = r;
    return GTEXT_CSS_OK;
  }
  default:
    *ok = false;
    return GTEXT_CSS_OK;
  }
}

/// A channel's value as the function's own clamp makes it (Color 4, 15; the same as the reader's for a literal): NaN is 0, an infinity the end of the range, a hue
/// is brought into [0, 360). A `color()` channel is not clamped.
static double clamp_channel(const css_color_fninfo * info, size_t i, bool color_fn, double v) {
  if (v != v) {
    return 0.0;
  }
  if (info->hue[i]) {
    return normalize_hue(v);
  }
  if (v > DBL_MAX || v < -DBL_MAX) {
    if (color_fn) {
      return v > 0.0 ? DBL_MAX : -DBL_MAX;
    }
    const double hi = info->hi[i] > DBL_MAX ? info->ref[i] : info->hi[i];
    const double lo = info->lo[i] < -DBL_MAX ? -info->ref[i] : info->lo[i];
    return v > 0.0 ? hi : lo;
  }
  if (color_fn) {
    return v;
  }
  return v < info->lo[i] ? info->lo[i] : v > info->hi[i] ? info->hi[i] : v;
}

/// The value of a function (`rgb()`, `lab()`, `color()` ...) in its own space. `origin` (in the function's space, or NULL) is the relative color's.
static GTEXT_CSS_Status eval_function(walk * w, const color_t * c, const css_cs * origin, css_cc_value * out, bool * ok) {
  css_color_fninfo info;
  *ok = false;
  if (!css_color_fn_info(c, &info)) {
    return GTEXT_CSS_OK;
  }
  const symenv se = {&info, origin};
  const symenv * sp = origin ? &se : NULL;
  /* Color 4, 15.4: the channels of color() are not clamped; nor are those of a relative rgb(), whose result is a color(srgb) that may be out of gamut (the pages: a round trip
     through rgb(from color(xyz-d65 ...) r g b) is the identity) */
  const bool color_fn = c->function == GTEXT_CSS_COLOR_FN_COLOR || (origin && c->function == GTEXT_CSS_COLOR_FN_RGB);
  css_cs r;
  memset(&r, 0, sizeof r);
  r.space = info.space;
  for (size_t i = 0; i < 3; i++) {
    double v;
    bool none, good;
    const GTEXT_CSS_Status s = comp_value(w, &c->components[i], info.ref[i], sp, &v, &none, &good);
    if (s != GTEXT_CSS_OK) {
      return s;
    }
    if (!good) {
      return GTEXT_CSS_OK;
    }
    r.none[i] = none;
    r.c[i] = none ? 0.0 : clamp_channel(&info, i, color_fn, v) / info.scale[i];
  }
  if (c->alpha_written) {
    double v;
    bool none, good;
    const GTEXT_CSS_Status s = comp_value(w, &c->alpha, 1.0, sp, &v, &none, &good);
    if (s != GTEXT_CSS_OK) {
      return s;
    }
    if (!good) {
      return GTEXT_CSS_OK;
    }
    r.none[3] = none;
    r.alpha = none ? 0.0 : clamp01(v);
  }
  else if (origin) {
    r.none[3] = origin->none[3];
    r.alpha = origin->alpha;
  }
  else {
    r.alpha = 1.0;
  }
  out->cs = r;
  out->legacy = !origin && !c->origin && !color_fn && (c->function == GTEXT_CSS_COLOR_FN_RGB || c->function == GTEXT_CSS_COLOR_FN_HSL || c->function == GTEXT_CSS_COLOR_FN_HWB);
  *ok = true;
  return GTEXT_CSS_OK;
}

/// The alpha of a literal color of the sRGB family (a named color, a hex color, `transparent`).
static double literal_alpha(const color_t * c) {
  if (c->alpha.kind == GTEXT_CSS_COMP_NUMBER) {
    return clamp01(c->alpha.value);
  }
  if (c->alpha.kind == GTEXT_CSS_COMP_PERCENT) {
    return clamp01(c->alpha.value / 100.0);
  }
  return 1.0;
}

/// A leaf of the tree. `inside_mix`: the leaf is an argument of a mix or the origin of a relative color.
static GTEXT_CSS_Status eval_leaf(walk * w, const color_t * c, bool inside_mix, css_cc_value * out, bool * ok) {
  css_color_ctx * ctx = w->ctx;
  ctx->work++;
  *ok = false;
  memset(out, 0, sizeof *out);
  switch (c->kind) {
  case GTEXT_CSS_COLOR_KEYWORD:
    if (c->keyword == GTEXT_CSS_COLOR_KW_NAMED || c->keyword == GTEXT_CSS_COLOR_KW_TRANSPARENT) {
      if (c->component_count != 3) {
        return GTEXT_CSS_OK;
      }
      out->cs.space = GTEXT_CSS_SPACE_SRGB;
      for (int i = 0; i < 3; i++) {
        out->cs.c[i] = c->components[i].value / 255.0;
      }
      out->cs.alpha = c->keyword == GTEXT_CSS_COLOR_KW_TRANSPARENT ? 0.0 : literal_alpha(c);
      out->legacy = true;
      *ok = true;
      return GTEXT_CSS_OK;
    }
    if (c->keyword == GTEXT_CSS_COLOR_KW_CURRENTCOLOR) {
      if (ctx->has_current && (!inside_mix || ctx->current_anywhere)) {
        *out = ctx->current;
        *ok = true;
      }
      return GTEXT_CSS_OK;
    }
    break;
  case GTEXT_CSS_COLOR_HEX:
    if (c->component_count != 3) {
      return GTEXT_CSS_OK;
    }
    out->cs.space = GTEXT_CSS_SPACE_SRGB;
    for (int i = 0; i < 3; i++) {
      out->cs.c[i] = c->components[i].value / 255.0;
    }
    out->cs.alpha = literal_alpha(c);
    out->legacy = true;
    *ok = true;
    return GTEXT_CSS_OK;
  case GTEXT_CSS_COLOR_FUNCTION:
    if (c->function != GTEXT_CSS_COLOR_FN_COLOR || c->space != GTEXT_CSS_SPACE_CUSTOM) {
      return eval_function(w, c, NULL, out, ok);
    }
    break;
  default:
    break;
  }
  /* a color nobody but the caller can compute: a system color, device-cmyk(), color() in a custom space */
  if (ctx->resolve) {
    double rgba[4];
    if (ctx->resolve(ctx->user, c, rgba)) {
      for (int k = 0; k < 4; k++) {
        if (!(rgba[k] - rgba[k] == 0.0)) {
          return GTEXT_CSS_OK;
        }
      }
      out->cs.space = GTEXT_CSS_SPACE_SRGB;
      for (int k = 0; k < 3; k++) {
        const double v = rgba[k] < 0.0 ? 0.0 : rgba[k] > 255.0 ? 255.0 : rgba[k];
        out->cs.c[k] = v / 255.0;
      }
      out->cs.alpha = clamp01(rgba[3]);
      out->legacy = true;
      *ok = true;
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- interpolation (Color 4, 13) ------------------------------------------------------------------------------------ */

static void fix_hues(double * h1, double * h2, GTEXT_CSS_Hue_Method m) {
  switch (m) {
  case GTEXT_CSS_HUE_LONGER:
    if (*h2 - *h1 > 0.0 && *h2 - *h1 < 180.0) {
      *h1 += 360.0;
    }
    else if (*h2 - *h1 > -180.0 && *h2 - *h1 <= 0.0) {
      *h2 += 360.0;
    }
    break;
  case GTEXT_CSS_HUE_INCREASING:
    if (*h2 < *h1) {
      *h2 += 360.0;
    }
    break;
  case GTEXT_CSS_HUE_DECREASING:
    if (*h1 < *h2) {
      *h1 += 360.0;
    }
    break;
  default: /* shorter, the default */
    if (*h2 - *h1 > 180.0) {
      *h1 += 360.0;
    }
    else if (*h2 - *h1 < -180.0) {
      *h2 += 360.0;
    }
    break;
  }
}

/// Interpolate `a` and `b`, both in the space `s`, at `p` (the share of `b`): Color 4, 13.3 (a missing component takes the other color's value; missing in both
/// stays missing), 13.4 (premultiplied, the hue not), 13.5.
static void interpolate(const css_cs * a, const css_cs * b, double p, GTEXT_CSS_Hue_Method method, css_cs * out) {
  css_cs x = *a, y = *b;
  const int hue = css_cs_hue_index(a->space);
  css_cs r;
  memset(&r, 0, sizeof r);
  r.space = a->space;
  for (int i = 0; i < 4; i++) {
    r.none[i] = x.none[i] && y.none[i];
  }
  for (int i = 0; i < 3; i++) {
    if (x.none[i] && !y.none[i]) {
      x.c[i] = y.c[i];
    }
    else if (y.none[i] && !x.none[i]) {
      y.c[i] = x.c[i];
    }
  }
  if (x.none[3] && !y.none[3]) {
    x.alpha = y.alpha;
  }
  else if (y.none[3] && !x.none[3]) {
    y.alpha = x.alpha;
  }
  if (hue >= 0 && !r.none[hue]) {
    fix_hues(&x.c[hue], &y.c[hue], method);
  }
  const bool premultiply = !r.none[3];
  for (int i = 0; i < 3; i++) {
    if (r.none[i]) {
      continue;
    }
    double u = x.c[i], v = y.c[i];
    if (premultiply && i != hue) {
      u *= x.alpha;
      v *= y.alpha;
    }
    r.c[i] = u * (1.0 - p) + v * p;
  }
  if (!r.none[3]) {
    r.alpha = x.alpha * (1.0 - p) + y.alpha * p;
    if (r.alpha != 0.0) {
      for (int i = 0; i < 3; i++) {
        if (!r.none[i] && i != hue) {
          r.c[i] /= r.alpha;
        }
      }
    }
  }
  if (hue >= 0 && !r.none[hue]) {
    r.c[hue] = normalize_hue(r.c[hue]);
  }
  *out = r;
}

/// Mix `n` colors (already in `space`) with the percentages `pct` (Color 5, 3.3). `pct` of a color without one is negative.
static void mix_items(const css_cc_value * items, double * pct, size_t n, GTEXT_CSS_Color_Space space, GTEXT_CSS_Hue_Method method, css_cc_value * out) {
  /* Values 5, 6.1 "Normalizing Mix Percentages", with the flag for forced normalization */
  double specified = 0.0;
  size_t omitted = 0;
  for (size_t i = 0; i < n; i++) {
    if (pct[i] >= 0.0) {
      specified += pct[i];
    }
    else {
      omitted++;
    }
  }
  if (specified > 100.0) {
    specified = 100.0;
  }
  if (omitted == n) {
    specified = 0.0;
  }
  double total = 0.0;
  for (size_t i = 0; i < n; i++) {
    if (pct[i] < 0.0) {
      pct[i] = (100.0 - specified) / (double)omitted;
    }
    total += pct[i];
  }
  double leftover = 0.0;
  if (total > 0.0) { /* more than 100%, or the forced normalization */
    for (size_t i = 0; i < n; i++) {
      pct[i] *= 100.0 / total;
    }
  }
  if (total < 100.0) {
    leftover = 100.0 - total;
  }
  const double alpha_mult = 1.0 - leftover / 100.0;
  css_cs acc;
  css_cs_convert(&items[0].cs, space, &acc);
  double acc_pct = pct[0];
  for (size_t i = 1; i < n; i++) {
    css_cs b;
    css_cs_convert(&items[i].cs, space, &b);
    const double combined = acc_pct + pct[i];
    const double progress = combined > 0.0 ? pct[i] / combined : 0.5;
    css_cs next;
    interpolate(&acc, &b, progress, method, &next);
    acc = next;
    acc_pct = combined;
  }
  if (!acc.none[3]) {
    acc.alpha = clamp01(acc.alpha * alpha_mult);
  }
  out->cs = acc;
  out->legacy = false;
}

/* ---- the nodes ------------------------------------------------------------------------------------------------------ */

static size_t kid_count(const color_t * c) {
  if (c->kind == GTEXT_CSS_COLOR_MIX) {
    return c->item_count;
  }
  if (c->kind == GTEXT_CSS_COLOR_CONTRAST || c->kind == GTEXT_CSS_COLOR_ALPHA || c->origin) {
    return 1;
  }
  return 0;
}

static const color_t * kid_at(const color_t * c, size_t i) {
  return c->kind == GTEXT_CSS_COLOR_MIX ? c->items[i] : c->origin;
}

/// The color a `light-dark()` stands for in the scheme (a chain of them is followed).
static const color_t * pick_scheme(const walk * w, const color_t * c) {
  size_t guard = 0;
  while (c && c->kind == GTEXT_CSS_COLOR_LIGHT_DARK && c->item_count == 2 && c->items[0] && c->items[1]) {
    c = c->items[w->ctx->scheme == GTEXT_CSS_SCHEME_DARK ? 1 : 0];
    if (++guard > 1000000) {
      break;
    }
  }
  return c;
}

/// Linear-light luminance of an sRGB color (WCAG 2.1, 1.4.3, "relative luminance"), the channels brought into gamut first.
static double luminance(const css_cs * v) {
  css_cs lin;
  css_cs_convert(v, GTEXT_CSS_SPACE_SRGB_LINEAR, &lin);
  double ch[3];
  for (int i = 0; i < 3; i++) {
    const double c = lin.none[i] ? 0.0 : lin.c[i];
    ch[i] = c < 0.0 ? 0.0 : c > 1.0 ? 1.0 : c;
  }
  return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
}

static GTEXT_CSS_Status finish_node(walk * w, const frame * f, bool * ok) {
  const color_t * c = f->c;
  css_color_ctx * ctx = w->ctx;
  css_cc_value * kids = w->vals + f->base;
  css_cc_value out;
  memset(&out, 0, sizeof out);
  *ok = true;
  ctx->work++;
  if (c->kind == GTEXT_CSS_COLOR_MIX) {
    if ((c->mix_has_space && c->mix_space == GTEXT_CSS_SPACE_CUSTOM) || c->item_count == 0) {
      *ok = false;
      return GTEXT_CSS_OK;
    }
    const size_t n = c->item_count;
    GTEXT_Allocator a = *ctx->alloc;
    /* the items of a mix are in memory already, one or more bytes of the text each, so the count times eight cannot wrap; the check says so */
    double * pct = n > SIZE_MAX / sizeof *pct ? NULL : gtext_allocator_malloc(&a, n * sizeof *pct);
    if (!pct) {
      return GTEXT_CSS_E_OOM;
    }
    for (size_t i = 0; i < n; i++) {
      const comp_t * p = &c->percents[i];
      double v;
      bool none, good;
      pct[i] = -1.0;
      if (p->kind == GTEXT_CSS_COMP_NONE) {
        continue;
      }
      const GTEXT_CSS_Status s = comp_value(w, p, 100.0, NULL, &v, &none, &good);
      if (s != GTEXT_CSS_OK) {
        gtext_allocator_free(&a, pct);
        return s;
      }
      if (!good) {
        gtext_allocator_free(&a, pct);
        *ok = false;
        return GTEXT_CSS_OK;
      }
      pct[i] = v != v ? 0.0 : v < 0.0 ? 0.0 : v > 100.0 ? 100.0 : v;
    }
    const GTEXT_CSS_Color_Space space = c->mix_has_space ? c->mix_space : GTEXT_CSS_SPACE_OKLAB;
    mix_items(kids, pct, n, space, c->hue_method, &out);
    gtext_allocator_free(&a, pct);
  }
  else if (c->kind == GTEXT_CSS_COLOR_CONTRAST) {
    /* "contrast-color() resolves to either white or black, whichever produces maximum color contrast ... If both white and black produce the same contrast, it resolves to
       white." (Color 5, 8). The algorithm is the user agent's; this one is the ratio of WCAG 2.1, 1.4.3, taken on the background's color without its alpha. */
    const double l = luminance(&kids[0].cs);
    const double against_white = 1.05 / (l + 0.05), against_black = (l + 0.05) / 0.05;
    const double v = against_white >= against_black ? 1.0 : 0.0;
    out.cs.space = GTEXT_CSS_SPACE_SRGB;
    out.cs.c[0] = out.cs.c[1] = out.cs.c[2] = v;
    out.cs.alpha = 1.0;
    out.legacy = true;
  }
  else if (c->kind == GTEXT_CSS_COLOR_ALPHA) {
    /* alpha(from <color> / <alpha>): "The result of this function is in the color space of the origin color." (Color 5, 4.10) */
    out = kids[0];
    out.legacy = false;
    css_color_fninfo info;
    memset(&info, 0, sizeof info);
    info.space = out.cs.space;
    info.sym[3] = "alpha";
    info.scale[0] = info.scale[1] = info.scale[2] = 1.0;
    const symenv se = {&info, &kids[0].cs};
    double v;
    bool none, good;
    const GTEXT_CSS_Status s = comp_value(w, &c->alpha, 1.0, &se, &v, &none, &good);
    if (s != GTEXT_CSS_OK) {
      return s;
    }
    if (!good) {
      *ok = false;
      return GTEXT_CSS_OK;
    }
    out.cs.none[3] = none;
    out.cs.alpha = none ? 0.0 : clamp01(v);
  }
  else {
    /* a relative function: the origin is converted to the function's space first (Color 5, 4.2) */
    css_color_fninfo info;
    if (!css_color_fn_info(c, &info)) {
      *ok = false;
      return GTEXT_CSS_OK;
    }
    css_cs o;
    css_cs_convert(&kids[0].cs, info.space, &o);
    const GTEXT_CSS_Status s = eval_function(w, c, &o, &out, ok);
    if (s != GTEXT_CSS_OK || !*ok) {
      return s;
    }
  }
  w->nval = f->base;
  return push_value(w, &out) ? GTEXT_CSS_OK : w->status;
}

GTEXT_CSS_Status css_color_compute(css_color_ctx * ctx, const GTEXT_CSS_Color * color, css_cc_value * out, bool * done) {
  walk w;
  memset(&w, 0, sizeof w);
  w.ctx = ctx;
  w.status = GTEXT_CSS_OK;
  *done = false;
  const color_t * root = pick_scheme(&w, color);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  bool ok = true;
  if (!root) {
    return GTEXT_CSS_OK;
  }
  const size_t rk = kid_count(root);
  if (rk == 0) {
    css_cc_value v;
    st = eval_leaf(&w, root, false, &v, &ok);
    if (st == GTEXT_CSS_OK && ok) {
      *out = v;
      *done = true;
    }
    return st;
  }
  if (!push_frame(&w, root, rk)) {
    st = w.status;
    goto out;
  }
  while (w.nfr > 0 && ok && st == GTEXT_CSS_OK) {
    frame * f = &w.fr[w.nfr - 1];
    if (f->next < f->kids) {
      const color_t * kid = pick_scheme(&w, kid_at(f->c, f->next++));
      if (!kid) {
        ok = false;
        break;
      }
      const size_t kk = kid_count(kid);
      if (kk > 0) {
        if (!push_frame(&w, kid, kk)) {
          st = w.status;
        }
        continue;
      }
      css_cc_value v;
      st = eval_leaf(&w, kid, true, &v, &ok);
      if (st == GTEXT_CSS_OK && ok && !push_value(&w, &v)) {
        st = w.status;
      }
      continue;
    }
    const frame done_frame = *f;
    w.nfr--;
    st = finish_node(&w, &done_frame, &ok);
  }
  if (st == GTEXT_CSS_OK && ok && w.nval == 1) {
    *out = w.vals[0];
    *done = true;
  }
out:
  {
    GTEXT_Allocator a = *ctx->alloc;
    gtext_allocator_free(&a, w.fr);
    gtext_allocator_free(&a, w.vals);
  }
  return st;
}

/* ---- the text of a computed color ----------------------------------------------------------------------------------- */

typedef struct {
  char * p;
  size_t n;
} wbuf;

static void put_str(wbuf * b, const char * s) {
  const size_t l = strlen(s);
  if (b->n + l < CSS_CC_TEXT_MAX) {
    memcpy(b->p + b->n, s, l);
    b->n += l;
  }
}

static void put_channel(wbuf * b, double v, bool none, const char * suffix) {
  if (none) {
    put_str(b, "none");
    return;
  }
  char num[352];
  const size_t l = css_cssom_number(v, false, num);
  num[l] = '\0';
  put_str(b, num);
  if (suffix) {
    put_str(b, suffix);
  }
}

static bool any_none(const css_cs * c) {
  return c->none[0] || c->none[1] || c->none[2] || c->none[3];
}

size_t css_color_value_text(const css_cc_value * v, char * out, css_cs * written) {
  wbuf b = {out, 0};
  css_cs c = v->cs;
  const char * fn = NULL;
  double scale[3] = {1.0, 1.0, 1.0};
  const char * suffix[3] = {NULL, NULL, NULL};
  bool as_color_fn = false;
  switch (c.space) {
  case GTEXT_CSS_SPACE_SRGB:
    if (v->legacy) {
      fn = "rgb";
      scale[0] = scale[1] = scale[2] = 255.0;
    }
    else {
      as_color_fn = true;
    }
    break;
  case GTEXT_CSS_SPACE_HSL:
  case GTEXT_CSS_SPACE_HWB:
    if (v->legacy || any_none(&c)) {
      fn = c.space == GTEXT_CSS_SPACE_HSL ? "hsl" : "hwb";
      suffix[1] = suffix[2] = "%";
    }
    else {
      css_cs t;
      css_cs_convert(&c, GTEXT_CSS_SPACE_SRGB, &t);
      c = t;
      as_color_fn = true;
    }
    break;
  case GTEXT_CSS_SPACE_LAB: fn = "lab"; break;
  case GTEXT_CSS_SPACE_LCH: fn = "lch"; break;
  case GTEXT_CSS_SPACE_OKLAB: fn = "oklab"; break;
  case GTEXT_CSS_SPACE_OKLCH: fn = "oklch"; break;
  default: as_color_fn = true; break;
  }
  if (as_color_fn) {
    put_str(&b, "color(");
    put_str(&b, gtext_css_color_space_name(c.space));
    put_str(&b, " ");
  }
  else {
    put_str(&b, fn);
    put_str(&b, "(");
  }
  for (int i = 0; i < 3; i++) {
    if (i) {
      put_str(&b, " ");
    }
    put_channel(&b, c.c[i] * scale[i], c.none[i], suffix[i]);
  }
  if (c.none[3] || c.alpha != 1.0) {
    put_str(&b, " / ");
    put_channel(&b, c.alpha, c.none[3], NULL);
  }
  put_str(&b, ")");
  out[b.n] = '\0';
  if (written) {
    *written = c;
  }
  return b.n;
}

GTEXT_CSS_Status gtext_css_color_compute_text(const GTEXT_CSS_Color * color, bool dark, const GTEXT_Allocator * alloc, char * out, size_t * length) {
  if (!color || !out || !length) {
    return GTEXT_CSS_E_INVALID;
  }
  *length = 0;
  out[0] = '\0';
  css_color_ctx cx;
  memset(&cx, 0, sizeof cx);
  cx.alloc = alloc ? alloc : gtext_allocator_default();
  cx.scheme = dark ? GTEXT_CSS_SCHEME_DARK : GTEXT_CSS_SCHEME_LIGHT;
  css_cc_value v;
  bool done = false;
  const GTEXT_CSS_Status s = css_color_compute(&cx, color, &v, &done);
  if (s != GTEXT_CSS_OK || !done) {
    return s;
  }
  css_cs written;
  *length = css_color_value_text(&v, out, &written);
  return GTEXT_CSS_OK;
}
