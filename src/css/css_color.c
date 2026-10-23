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
 * Reading a color from a component value (CSS Color 4 sections 4 to 9, CSS Color 5
 * sections 2 to 8).
 *
 * A color is always one component value, so a color nested in a color (`color-mix()`,
 * `light-dark()`, `contrast-color()`, the origin of a relative color) is found by a
 * shallow scan of the function's arguments, built first (the frames of an explicit
 * stack; the nesting is a limit, GTEXT_CSS_E_LIMIT_COLOR_NESTING), and then handed to
 * the node that holds it. What is kept is what was written; see css_color.h.
 *
 * The parsed-value-time rules are the draft's: an `rgb()` channel is clamped to 0 up to 255, a negative
 * `hsl()` saturation and a negative chroma to 0 (the whiteness and blackness of `hwb()` are not clamped), the lightness of `lab()`, `lch()`, `oklab()`
 * and `oklch()` to 0 up to its reference maximum (the lightness of `hsl()` and the channels of `color()` are not clamped), an alpha to
 * 0 up to 1, and a hue is normalized to 0 up to 360; "when calc() is simplified down to a single value" in an sRGB color it is clamped
 * (infinity to the upper bound, minus infinity and NaN to the lower), while a math function in
 * `lab()`, `lch()`, `oklab()`, `oklch()` and `color()` is kept as written.
 */

#include <ghoti.io/math/basic.h>

#include <float.h>
#include <string.h>

#include "css_color_internal.h"

#define DEFAULT_MAX_NESTING ((size_t)32)
#define LIMIT_MAX_NESTING ((size_t)4096)
#define MAX_CUSTOM_COMPONENTS 1024

GTEXT_CSS_Color_Options gtext_css_color_options_default(void) {
  GTEXT_CSS_Color_Options o;
  memset(&o, 0, sizeof o);
  o.max_nesting = DEFAULT_MAX_NESTING;
  return o;
}

typedef GTEXT_CSS_Color color_t;
typedef GTEXT_CSS_Color_Component comp_t;
typedef GTEXT_CSS_Component_Value cv_t;

typedef struct {
  css_arena * arena;
  GTEXT_Allocator alloc;
  size_t max_nesting, max_math_depth, max_math_nodes;
  size_t work, nodes;
  GTEXT_CSS_Status status;
  GTEXT_CSS_Span where;
} cb_t;

static void cb_fail(cb_t * cb, GTEXT_CSS_Status st) {
  if (cb->status == GTEXT_CSS_OK) {
    cb->status = st;
  }
}

/* ---- names ------------------------------------------------------------------------ */

static char fold1(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool name_is(const GTEXT_CSS_String * s, const char * lower) {
  const size_t n = strlen(lower);
  if (s->length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (fold1(s->data[i]) != lower[i]) {
      return false;
    }
  }
  return true;
}

typedef enum {
  F_NONE = 0, F_RGB, F_HSL, F_HWB, F_LAB, F_LCH, F_OKLAB, F_OKLCH, F_COLOR, F_MIX, F_LIGHT_DARK, F_CONTRAST, F_ALPHA, F_CMYK
} fid;

static fid fid_of(const GTEXT_CSS_String * name) {
  static const struct { const char * n; fid f; } T[] = {
    {"rgb", F_RGB}, {"rgba", F_RGB}, {"hsl", F_HSL}, {"hsla", F_HSL}, {"hwb", F_HWB}, {"lab", F_LAB}, {"lch", F_LCH},
    {"oklab", F_OKLAB}, {"oklch", F_OKLCH}, {"color", F_COLOR}, {"color-mix", F_MIX}, {"light-dark", F_LIGHT_DARK},
    {"contrast-color", F_CONTRAST}, {"alpha", F_ALPHA}, {"device-cmyk", F_CMYK},
  };
  for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
    if (name_is(name, T[i].n)) {
      return T[i].f;
    }
  }
  return F_NONE;
}

bool css_color_is_candidate(const cv_t * v) {
  if (v->kind == GTEXT_CSS_VALUE_FUNCTION) {
    return fid_of(&v->value) != F_NONE;
  }
  return v->kind == GTEXT_CSS_VALUE_TOKEN && (v->type == GTEXT_CSS_TOKEN_IDENT || v->type == GTEXT_CSS_TOKEN_HASH);
}

static const struct { const char * n; GTEXT_CSS_Color_Space s; } SPACES[] = {
  {"srgb", GTEXT_CSS_SPACE_SRGB}, {"srgb-linear", GTEXT_CSS_SPACE_SRGB_LINEAR}, {"display-p3", GTEXT_CSS_SPACE_DISPLAY_P3},
  {"display-p3-linear", GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR}, {"a98-rgb", GTEXT_CSS_SPACE_A98_RGB},
  {"prophoto-rgb", GTEXT_CSS_SPACE_PROPHOTO_RGB}, {"rec2020", GTEXT_CSS_SPACE_REC2020}, {"xyz", GTEXT_CSS_SPACE_XYZ_D65},
  {"xyz-d50", GTEXT_CSS_SPACE_XYZ_D50}, {"xyz-d65", GTEXT_CSS_SPACE_XYZ_D65}, {"lab", GTEXT_CSS_SPACE_LAB},
  {"lch", GTEXT_CSS_SPACE_LCH}, {"oklab", GTEXT_CSS_SPACE_OKLAB}, {"oklch", GTEXT_CSS_SPACE_OKLCH},
  {"hsl", GTEXT_CSS_SPACE_HSL}, {"hwb", GTEXT_CSS_SPACE_HWB},
};

const char * gtext_css_color_space_name(GTEXT_CSS_Color_Space space) {
  switch (space) {
  case GTEXT_CSS_SPACE_SRGB: return "srgb";
  case GTEXT_CSS_SPACE_SRGB_LINEAR: return "srgb-linear";
  case GTEXT_CSS_SPACE_DISPLAY_P3: return "display-p3";
  case GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR: return "display-p3-linear";
  case GTEXT_CSS_SPACE_A98_RGB: return "a98-rgb";
  case GTEXT_CSS_SPACE_PROPHOTO_RGB: return "prophoto-rgb";
  case GTEXT_CSS_SPACE_REC2020: return "rec2020";
  case GTEXT_CSS_SPACE_XYZ_D50: return "xyz-d50";
  case GTEXT_CSS_SPACE_XYZ_D65: return "xyz-d65";
  case GTEXT_CSS_SPACE_LAB: return "lab";
  case GTEXT_CSS_SPACE_LCH: return "lch";
  case GTEXT_CSS_SPACE_OKLAB: return "oklab";
  case GTEXT_CSS_SPACE_OKLCH: return "oklch";
  case GTEXT_CSS_SPACE_HSL: return "hsl";
  case GTEXT_CSS_SPACE_HWB: return "hwb";
  default: return NULL;
  }
}

static bool space_of(const GTEXT_CSS_String * s, GTEXT_CSS_Color_Space * out) {
  for (size_t i = 0; i < sizeof SPACES / sizeof SPACES[0]; i++) {
    if (name_is(s, SPACES[i].n)) {
      *out = SPACES[i].s;
      return true;
    }
  }
  return false;
}

static bool is_dashed(const GTEXT_CSS_String * s) {
  return s->length >= 3 && s->data[0] == '-' && s->data[1] == '-';
}

/* ---- the arena helpers ---------------------------------------------------------------- */

static void * zalloc(cb_t * cb, size_t n) {
  if (n == 0) {
    return NULL;
  }
  void * p = css_arena_alloc(cb->arena, n);
  if (!p) {
    cb_fail(cb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  memset(p, 0, n);
  return p;
}

static color_t * new_color(cb_t * cb, GTEXT_CSS_Color_Kind kind, const cv_t * v) {
  color_t * c = zalloc(cb, sizeof *c);
  if (!c) {
    return NULL;
  }
  c->kind = kind;
  c->span = v->span;
  c->alpha.kind = GTEXT_CSS_COMP_NUMBER;
  c->alpha.value = 1.0;
  cb->nodes++;
  return c;
}

static bool copy_string(cb_t * cb, const char * s, size_t n, GTEXT_CSS_String * out) {
  char * p = css_arena_strdup(cb->arena, s, n);
  if (!p) {
    cb_fail(cb, GTEXT_CSS_E_OOM);
    return false;
  }
  out->data = p;
  out->length = n;
  return true;
}

/* ---- the arguments of a function ----------------------------------------------------- */

typedef struct {
  const cv_t ** v;
  size_t n;
} args_t;

static bool is_ws(const cv_t * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_tok(const cv_t * v, GTEXT_CSS_Token_Type t) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == t;
}

static bool is_slash(const cv_t * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == '/';
}

static bool is_ident(const cv_t * v, const char * lower) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT && name_is(&v->value, lower);
}

/// The component values of a function's arguments with white space left out (allocated from the scratch allocator).
static bool collect_args(cb_t * cb, const cv_t * fn, args_t * out) {
  out->v = NULL;
  out->n = 0;
  size_t n = 0;
  for (size_t i = 0; i < fn->child_count; i++) {
    if (!is_ws(&fn->children[i])) {
      n++;
    }
  }
  cb->work += fn->child_count + 1;
  if (n == 0) {
    return true;
  }
  const cv_t ** v = gtext_allocator_malloc(&cb->alloc, n * sizeof *v);
  if (!v) {
    cb_fail(cb, GTEXT_CSS_E_OOM);
    return false;
  }
  size_t k = 0;
  for (size_t i = 0; i < fn->child_count; i++) {
    if (!is_ws(&fn->children[i])) {
      v[k++] = &fn->children[i];
    }
  }
  out->v = v;
  out->n = n;
  return true;
}

static void free_args(cb_t * cb, args_t * a) {
  gtext_allocator_free(&cb->alloc, a->v);
  a->v = NULL;
  a->n = 0;
}

/// The arguments separated by commas: `starts[i]` and `ends[i]` (exclusive) index into `a`. Returns the count, or (size_t)-1 on OOM.
static size_t split_commas(cb_t * cb, const args_t * a, size_t from, size_t ** starts, size_t ** ends) {
  size_t groups = 1;
  for (size_t i = from; i < a->n; i++) {
    if (is_tok(a->v[i], GTEXT_CSS_TOKEN_COMMA)) {
      groups++;
    }
  }
  size_t * s = gtext_allocator_malloc(&cb->alloc, groups * 2 * sizeof *s);
  if (!s) {
    cb_fail(cb, GTEXT_CSS_E_OOM);
    return (size_t)-1;
  }
  size_t g = 0, begin = from;
  for (size_t i = from; i <= a->n; i++) {
    if (i == a->n || is_tok(a->v[i], GTEXT_CSS_TOKEN_COMMA)) {
      s[g] = begin;
      s[groups + g] = i;
      g++;
      begin = i + 1;
    }
  }
  *starts = s;
  *ends = s + groups;
  return groups;
}

/// A component value that is a percentage in a `color-mix()` item: a percentage token or a math function.
static bool looks_like_percentage(const cv_t * v) {
  if (is_tok(v, GTEXT_CSS_TOKEN_PERCENTAGE)) {
    return true;
  }
  return v->kind == GTEXT_CSS_VALUE_FUNCTION && css_math_accepts_function(v->value.data, v->value.length);
}

/* ---- the colors nested in a color: the shallow scan -------------------------------------- */

typedef struct {
  const cv_t ** v;
  size_t n, cap;
} cvvec;

static bool cvvec_push(cb_t * cb, cvvec * vec, const cv_t * v) {
  if (vec->n == vec->cap) {
    const size_t cap = vec->cap ? vec->cap * 2 : 8;
    const cv_t ** p = gtext_allocator_realloc(&cb->alloc, vec->v, cap * sizeof *p);
    if (!p) {
      cb_fail(cb, GTEXT_CSS_E_OOM);
      return false;
    }
    vec->v = p;
    vec->cap = cap;
  }
  vec->v[vec->n++] = v;
  return true;
}

/// Find the colors a function holds: the origin after `from`, the items of `color-mix()`, the two of `light-dark()`, the one of `contrast-color()`.
static bool scan_children(cb_t * cb, const cv_t * fn, cvvec * out) {
  if (fn->kind != GTEXT_CSS_VALUE_FUNCTION) {
    return true;
  }
  const fid f = fid_of(&fn->value);
  if (f == F_NONE) {
    return true;
  }
  args_t a;
  if (!collect_args(cb, fn, &a)) {
    return false;
  }
  bool ok = true;
  if (f == F_MIX || f == F_LIGHT_DARK || f == F_CONTRAST) {
    size_t from = 0;
    if (f == F_MIX && a.n > 0 && is_ident(a.v[0], "in")) {
      /* the interpolation method runs to the first comma */
      while (from < a.n && !is_tok(a.v[from], GTEXT_CSS_TOKEN_COMMA)) {
        from++;
      }
      from++;
    }
    size_t * starts = NULL, * ends = NULL;
    const size_t groups = from <= a.n ? split_commas(cb, &a, from, &starts, &ends) : 0;
    if (groups == (size_t)-1) {
      ok = false;
    }
    for (size_t g = 0; ok && g < groups; g++) {
      const size_t len = ends[g] - starts[g];
      const cv_t * color = NULL;
      if (len == 1) {
        color = a.v[starts[g]];
      }
      else if (len == 2 && f == F_MIX) {
        color = looks_like_percentage(a.v[starts[g]]) ? a.v[starts[g] + 1] : a.v[starts[g]];
      }
      if (color && css_color_is_candidate(color)) {
        ok = cvvec_push(cb, out, color);
      }
    }
    gtext_allocator_free(&cb->alloc, starts);
  }
  else if (f != F_CMYK && a.n >= 2 && is_ident(a.v[0], "from")) {
    /* (device-cmyk() has no relative form: its `from` is no origin, and the builder takes no color from it) */
    if (css_color_is_candidate(a.v[1])) {
      ok = cvvec_push(cb, out, a.v[1]);
    }
  }
  free_args(cb, &a);
  return ok;
}

/* ---- reading components ----------------------------------------------------------------------- */

typedef struct {
  unsigned num : 1, pct : 1, hue : 1, none : 1;
} slot_t;

static const slot_t SLOT_NP = {1, 1, 0, 1};      ///< number, percentage or none
static const slot_t SLOT_ALPHA = {1, 1, 0, 1};

/// The channel keywords of a relative color: three and `alpha`.
typedef struct {
  const char * const * names;
  size_t count;
} symbols_t;

static const GTEXT_CSS_Math_Type NUMBER_TYPE = {{0, 0, 0, 0, 0, 0, 0}, 0};

/// Read one component. 1: read; 0: not valid; -1: a hard failure (the status is set).
static int read_comp(cb_t * cb, const cv_t * v, slot_t slot, const symbols_t * sym, comp_t * out) {
  memset(out, 0, sizeof *out);
  cb->work++;
  if (v->kind == GTEXT_CSS_VALUE_TOKEN) {
    switch (v->type) {
    case GTEXT_CSS_TOKEN_NUMBER:
      if (!slot.num) {
        return 0;
      }
      out->kind = GTEXT_CSS_COMP_NUMBER;
      out->value = v->number;
      return 1;
    case GTEXT_CSS_TOKEN_PERCENTAGE:
      if (!slot.pct) {
        return 0;
      }
      out->kind = GTEXT_CSS_COMP_PERCENT;
      out->value = v->number;
      return 1;
    case GTEXT_CSS_TOKEN_DIMENSION: {
      if (!slot.hue) {
        return 0;
      }
      const GTEXT_CSS_Unit u = gtext_css_unit_lookup(v->unit.data, v->unit.length);
      double deg;
      if (gtext_css_unit_type(u) != GTEXT_CSS_UNIT_TYPE_ANGLE || !gtext_css_unit_to_canonical(u, v->number, &deg)) {
        return 0;
      }
      out->kind = GTEXT_CSS_COMP_ANGLE;
      out->value = deg;
      return 1;
    }
    case GTEXT_CSS_TOKEN_IDENT:
      if (slot.none && name_is(&v->value, "none")) {
        out->kind = GTEXT_CSS_COMP_NONE;
        return 1;
      }
      if (sym) {
        for (size_t i = 0; i < sym->count; i++) {
          if (name_is(&v->value, sym->names[i])) {
            out->kind = GTEXT_CSS_COMP_SYMBOL;
            return copy_string(cb, sym->names[i], strlen(sym->names[i]), &out->text) ? 1 : -1;
          }
        }
      }
      return 0;
    default:
      return 0;
    }
  }
  if (v->kind == GTEXT_CSS_VALUE_FUNCTION && css_math_accepts_function(v->value.data, v->value.length)) {
    GTEXT_CSS_Math_Context mc;
    GTEXT_CSS_Math_Keyword kw[8];
    memset(&mc, 0, sizeof mc);
    if (sym && sym->count <= 8) {
      for (size_t i = 0; i < sym->count; i++) {
        kw[i].name = sym->names[i];
        kw[i].type = NUMBER_TYPE;
      }
      mc.keywords = kw;
      mc.keyword_count = sym->count;
    }
    GTEXT_CSS_Math * m = NULL;
    bool valid = false;
    GTEXT_CSS_Math_Result res;
    const GTEXT_CSS_Status st = css_math_build(cb->arena, &cb->alloc, v, &mc, cb->max_math_depth, cb->max_math_nodes, &m, &valid, &res);
    if (st != GTEXT_CSS_OK) {
      cb_fail(cb, st);
      cb->where = res.where;
      return -1;
    }
    cb->work += res.work;
    if (!valid) {
      return 0;
    }
    const bool ok = (m->category == GTEXT_CSS_MATH_CAT_NUMBER && (slot.num || slot.hue)) ||
        (m->category == GTEXT_CSS_MATH_CAT_PERCENTAGE && slot.pct) ||
        (m->category == GTEXT_CSS_MATH_CAT_ANGLE && slot.hue);
    if (!ok || (m->has_percent && !slot.pct)) {
      return 0;
    }
    out->kind = GTEXT_CSS_COMP_MATH;
    out->math = m;
    return 1;
  }
  return 0;
}

/// The sRGB-family rule: a math function that came down to one value becomes that value (censored: NaN and minus
/// infinity to the lower bound, infinity to the upper).
static void resolve_math(comp_t * c, bool hue, double lo_inf, double hi_inf) {
  if (c->kind != GTEXT_CSS_COMP_MATH) {
    return;
  }
  double v;
  GTEXT_CSS_Unit u;
  bool pct;
  if (!gtext_css_math_numeric(c->math, &v, &u, &pct)) {
    return;
  }
  if (u != GTEXT_CSS_UNIT_NONE && !gtext_css_unit_is_absolute(u)) {
    return;
  }
  const GTEXT_CSS_Math_Category cat = c->math->category;
  if (v != v) {
    v = hue ? 0.0 : lo_inf;
  }
  else if (v > DBL_MAX) {
    v = hue ? 0.0 : hi_inf;
  }
  else if (v < -DBL_MAX) {
    v = hue ? 0.0 : lo_inf;
  }
  c->kind = cat == GTEXT_CSS_MATH_CAT_PERCENTAGE ? GTEXT_CSS_COMP_PERCENT : cat == GTEXT_CSS_MATH_CAT_ANGLE ? GTEXT_CSS_COMP_ANGLE : GTEXT_CSS_COMP_NUMBER;
  c->value = v;
  c->math = NULL;
}

static double clamp_to(double x, double lo, double hi) {
  return x < lo ? lo : x > hi ? hi : x;
}

static double normalize_hue(double h) {
  if (h != h || h > DBL_MAX || h < -DBL_MAX) {
    return 0.0;
  }
  double r = css_math_fmod_exact(h, 360.0);
  if (r < 0.0) {
    r += 360.0;
  }
  if (r >= 360.0 || r == 0.0) {
    r = 0.0;
  }
  return r;
}

/// A channel's reference range, and the clamp of a literal.
typedef struct {
  double ref;       ///< 100% is this many.
  double lo, hi;    ///< Clamp of a literal number.
  bool hue;
} chan_t;

#define INF_ (1.0 / 0.0)

static void apply_channel(comp_t * c, const chan_t * ch, bool resolve, bool clamp) {
  if (ch->hue) {
    if (resolve) {
      resolve_math(c, true, 0.0, 0.0);
    }
    if (clamp && (c->kind == GTEXT_CSS_COMP_NUMBER || c->kind == GTEXT_CSS_COMP_ANGLE)) {
      c->value = normalize_hue(c->value);
    }
    return;
  }
  if (resolve && c->kind == GTEXT_CSS_COMP_MATH) {
    /* censor to the channel's own range (percent points for a percentage) */
    double v;
    GTEXT_CSS_Unit u;
    bool pct;
    if (gtext_css_math_numeric(c->math, &v, &u, &pct) && (u == GTEXT_CSS_UNIT_NONE || gtext_css_unit_is_absolute(u))) {
      const double scale = pct ? 100.0 / ch->ref : 1.0;
      const double lo = ch->lo == -INF_ ? -ch->ref : ch->lo, hi = ch->hi == INF_ ? ch->ref : ch->hi;
      resolve_math(c, false, lo * scale, hi * scale);
    }
  }
  if (clamp && c->kind == GTEXT_CSS_COMP_NUMBER) {
    c->value = clamp_to(c->value, ch->lo, ch->hi);
  }
  else if (clamp && c->kind == GTEXT_CSS_COMP_PERCENT) {
    c->value = clamp_to(c->value, ch->lo / ch->ref * 100.0, ch->hi / ch->ref * 100.0);
  }
}

/* ---- the function descriptors ----------------------------------------------------------------- */

typedef struct {
  fid id;
  const char * name;
  GTEXT_CSS_Color_Function fn;
  GTEXT_CSS_Color_Space space;
  slot_t slot[3];
  chan_t chan[3];
  const char * sym[4];
  bool srgb_family;
} fdesc;

static const fdesc FD_RGB = {F_RGB, "rgb", GTEXT_CSS_COLOR_FN_RGB, GTEXT_CSS_SPACE_SRGB,
  {{1, 1, 0, 1}, {1, 1, 0, 1}, {1, 1, 0, 1}}, {{255.0, 0.0, 255.0, false}, {255.0, 0.0, 255.0, false}, {255.0, 0.0, 255.0, false}},
  {"r", "g", "b", "alpha"}, true};
static const fdesc FD_HSL = {F_HSL, "hsl", GTEXT_CSS_COLOR_FN_HSL, GTEXT_CSS_SPACE_HSL,
  {{1, 0, 1, 1}, {1, 1, 0, 1}, {1, 1, 0, 1}}, {{360.0, 0.0, 0.0, true}, {100.0, 0.0, 1.0 / 0.0, false}, {100.0, -1.0 / 0.0, 1.0 / 0.0, false}},
  {"h", "s", "l", "alpha"}, true};
static const fdesc FD_HWB = {F_HWB, "hwb", GTEXT_CSS_COLOR_FN_HWB, GTEXT_CSS_SPACE_HWB,
  {{1, 0, 1, 1}, {1, 1, 0, 1}, {1, 1, 0, 1}}, {{360.0, 0.0, 0.0, true}, {100.0, -1.0 / 0.0, 1.0 / 0.0, false}, {100.0, -1.0 / 0.0, 1.0 / 0.0, false}},
  {"h", "w", "b", "alpha"}, true};
static const fdesc FD_LAB = {F_LAB, "lab", GTEXT_CSS_COLOR_FN_LAB, GTEXT_CSS_SPACE_LAB,
  {{1, 1, 0, 1}, {1, 1, 0, 1}, {1, 1, 0, 1}}, {{100.0, 0.0, 100.0, false}, {125.0, -1.0 / 0.0, 1.0 / 0.0, false}, {125.0, -1.0 / 0.0, 1.0 / 0.0, false}},
  {"l", "a", "b", "alpha"}, false};
static const fdesc FD_LCH = {F_LCH, "lch", GTEXT_CSS_COLOR_FN_LCH, GTEXT_CSS_SPACE_LCH,
  {{1, 1, 0, 1}, {1, 1, 0, 1}, {1, 0, 1, 1}}, {{100.0, 0.0, 100.0, false}, {150.0, 0.0, 1.0 / 0.0, false}, {360.0, 0.0, 0.0, true}},
  {"l", "c", "h", "alpha"}, false};
static const fdesc FD_OKLAB = {F_OKLAB, "oklab", GTEXT_CSS_COLOR_FN_OKLAB, GTEXT_CSS_SPACE_OKLAB,
  {{1, 1, 0, 1}, {1, 1, 0, 1}, {1, 1, 0, 1}}, {{1.0, 0.0, 1.0, false}, {0.4, -1.0 / 0.0, 1.0 / 0.0, false}, {0.4, -1.0 / 0.0, 1.0 / 0.0, false}},
  {"l", "a", "b", "alpha"}, false};
static const fdesc FD_OKLCH = {F_OKLCH, "oklch", GTEXT_CSS_COLOR_FN_OKLCH, GTEXT_CSS_SPACE_OKLCH,
  {{1, 1, 0, 1}, {1, 1, 0, 1}, {1, 0, 1, 1}}, {{1.0, 0.0, 1.0, false}, {0.4, 0.0, 1.0 / 0.0, false}, {360.0, 0.0, 0.0, true}},
  {"l", "c", "h", "alpha"}, false};

static const fdesc * fdesc_of(fid f) {
  switch (f) {
  case F_RGB: return &FD_RGB;
  case F_HSL: return &FD_HSL;
  case F_HWB: return &FD_HWB;
  case F_LAB: return &FD_LAB;
  case F_LCH: return &FD_LCH;
  case F_OKLAB: return &FD_OKLAB;
  case F_OKLCH: return &FD_OKLCH;
  default: return NULL;
  }
}

static const chan_t ALPHA_CHAN = {1.0, 0.0, 1.0, false};

static void note_flags(color_t * c) {
  c->has_missing = false;
  c->has_unresolved = false;
  for (size_t i = 0; i <= c->component_count; i++) {
    const comp_t * k = i < c->component_count ? &c->components[i] : &c->alpha;
    if (k->kind == GTEXT_CSS_COMP_NONE) {
      c->has_missing = true;
    }
    if (k->kind == GTEXT_CSS_COMP_SYMBOL || k->kind == GTEXT_CSS_COMP_MATH) {
      c->has_unresolved = true;
    }
  }
}

/// The alpha: a number, a percentage, none, a math function, or a channel keyword. Clamped, or censored, for what it is.
static int read_alpha(cb_t * cb, const cv_t * v, bool allow_none, const symbols_t * sym, bool srgb_family, bool relative, color_t * c) {
  slot_t slot = SLOT_ALPHA;
  slot.none = allow_none;
  const int r = read_comp(cb, v, slot, sym, &c->alpha);
  if (r != 1) {
    return r;
  }
  c->alpha_written = true;
  if (!relative) {
    apply_channel(&c->alpha, &ALPHA_CHAN, srgb_family, true);
  }
  return 1;
}

/// Read the channels (and alpha) of a modern function: `[from X]? c1 c2 c3 [/ alpha]?`. `a` starts at the channels.
static int read_modern(cb_t * cb, const fdesc * fd, const args_t * a, size_t first, bool relative, color_t * c) {
  const size_t rest = a->n - first;
  if (rest != 3 && !(rest == 5 && is_slash(a->v[first + 3]))) {
    return 0;
  }
  const symbols_t sym = {fd->sym, 4};
  const symbols_t * sp = relative ? &sym : NULL;
  c->components = zalloc(cb, 3 * sizeof *c->components);
  if (!c->components) {
    return -1;
  }
  c->component_count = 3;
  for (size_t i = 0; i < 3; i++) {
    const int r = read_comp(cb, a->v[first + i], fd->slot[i], sp, &c->components[i]);
    if (r != 1) {
      return r;
    }
    if (!relative) {
      apply_channel(&c->components[i], &fd->chan[i], fd->srgb_family, true);
    }
  }
  if (rest == 5) {
    const int r = read_alpha(cb, a->v[first + 4], true, sp, fd->srgb_family, relative, c);
    if (r != 1) {
      return r;
    }
  }
  return 1;
}

/// The legacy comma syntax of rgb() and hsl(): `c1 , c2 , c3 [, alpha]?`.
static int read_legacy(cb_t * cb, const fdesc * fd, const args_t * a, color_t * c) {
  if ((a->n != 5 && a->n != 7) || !is_tok(a->v[1], GTEXT_CSS_TOKEN_COMMA) || !is_tok(a->v[3], GTEXT_CSS_TOKEN_COMMA) ||
      (a->n == 7 && !is_tok(a->v[5], GTEXT_CSS_TOKEN_COMMA))) {
    return 0;
  }
  c->legacy = true;
  c->components = zalloc(cb, 3 * sizeof *c->components);
  if (!c->components) {
    return -1;
  }
  c->component_count = 3;
  bool any_pct = false, any_num = false;
  for (size_t i = 0; i < 3; i++) {
    slot_t slot = fd->slot[i];
    slot.none = 0;
    if (fd->id == F_HSL && i > 0) {
      slot.num = 0; /* the saturation and lightness of the legacy syntax are percentages */
    }
    const int r = read_comp(cb, a->v[i * 2], slot, NULL, &c->components[i]);
    if (r != 1) {
      return r;
    }
    if (fd->id == F_RGB) {
      const comp_t * k = &c->components[i];
      const bool pct = k->kind == GTEXT_CSS_COMP_PERCENT || (k->kind == GTEXT_CSS_COMP_MATH && k->math->category == GTEXT_CSS_MATH_CAT_PERCENTAGE);
      if (pct) {
        any_pct = true;
      }
      else {
        any_num = true;
      }
    }
    apply_channel(&c->components[i], &fd->chan[i], true, true);
  }
  if (any_pct && any_num) {
    return 0; /* "rgb( <percentage>#{3} , ...) | rgb( <number>#{3} , ...)": no mixing in the legacy syntax */
  }
  if (a->n == 7) {
    const int r = read_alpha(cb, a->v[6], false, NULL, true, false, c);
    if (r != 1) {
      return r;
    }
  }
  return 1;
}

/* ---- building one node ------------------------------------------------------------------------ */

/// Where the children of the node being built are: handed out in the order the scan found them.
typedef struct {
  color_t * const * kids;
  size_t n, next;
  const cv_t * const * cvs;  ///< The component value each kid was built from (the scan's list, in the same order).
} kidlist;

/// The next kid, if it is the color of `expect`: the builder and the scan must agree on which component value is a color, so a kid
/// made from another one (or none) is no color here.
static color_t * take_kid(kidlist * k, const cv_t * expect) {
  if (k->next >= k->n || k->cvs[k->next] != expect) {
    return NULL;
  }
  return k->kids[k->next++];
}

static color_t * build_function(cb_t * cb, const cv_t * v, fid f, kidlist * kids) {
  args_t a;
  if (!collect_args(cb, v, &a)) {
    return NULL;
  }
  color_t * c = NULL;
  bool ok = false;
  size_t first = 0;
  const bool relative = a.n >= 2 && is_ident(a.v[0], "from") && f != F_MIX && f != F_LIGHT_DARK && f != F_CONTRAST && f != F_CMYK;
  if (f == F_RGB || f == F_HSL || f == F_HWB || f == F_LAB || f == F_LCH || f == F_OKLAB || f == F_OKLCH) {
    const fdesc * fd = fdesc_of(f);
    c = new_color(cb, GTEXT_CSS_COLOR_FUNCTION, v);
    if (!c) {
      goto done;
    }
    c->function = fd->fn;
    c->space = fd->space;
    int r;
    bool has_commas = false;
    for (size_t i = 0; i < a.n; i++) {
      if (is_tok(a.v[i], GTEXT_CSS_TOKEN_COMMA)) {
        has_commas = true;
      }
    }
    if (relative) {
      c->origin = take_kid(kids, a.v[1]);
      if (!c->origin) {
        goto done;
      }
      first = 2;
    }
    if (has_commas) {
      if (relative || (f != F_RGB && f != F_HSL)) {
        goto done;
      }
      r = read_legacy(cb, fd, &a, c);
    }
    else {
      r = read_modern(cb, fd, &a, first, relative, c);
    }
    ok = r == 1;
  }
  else if (f == F_COLOR) {
    c = new_color(cb, GTEXT_CSS_COLOR_FUNCTION, v);
    if (!c) {
      goto done;
    }
    c->function = GTEXT_CSS_COLOR_FN_COLOR;
    if (relative) {
      c->origin = take_kid(kids, a.v[1]);
      if (!c->origin) {
        goto done;
      }
      first = 2;
    }
    if (a.n <= first || a.v[first]->kind != GTEXT_CSS_VALUE_TOKEN || a.v[first]->type != GTEXT_CSS_TOKEN_IDENT) {
      goto done;
    }
    const GTEXT_CSS_String * sp = &a.v[first]->value;
    size_t slash = a.n;
    for (size_t i = first + 1; i < a.n; i++) {
      if (is_slash(a.v[i])) {
        slash = i;
        break;
      }
    }
    const size_t ncomp = slash - (first + 1);
    const char * rgb_names[] = {"r", "g", "b", "alpha"};
    const char * xyz_names[] = {"x", "y", "z", "alpha"};
    symbols_t sym = {rgb_names, 4};
    GTEXT_CSS_Color_Space space;
    if (space_of(sp, &space) && space != GTEXT_CSS_SPACE_LAB && space != GTEXT_CSS_SPACE_LCH && space != GTEXT_CSS_SPACE_OKLAB &&
        space != GTEXT_CSS_SPACE_OKLCH && space != GTEXT_CSS_SPACE_HSL && space != GTEXT_CSS_SPACE_HWB) {
      if (ncomp != 3) {
        goto done;
      }
      if (space == GTEXT_CSS_SPACE_XYZ_D50 || space == GTEXT_CSS_SPACE_XYZ_D65) {
        sym.names = xyz_names;
      }
    }
    else if (is_dashed(sp)) {
      space = GTEXT_CSS_SPACE_CUSTOM;
      if (ncomp < 1 || ncomp > MAX_CUSTOM_COMPONENTS || relative) {
        goto done;
      }
      if (!copy_string(cb, sp->data, sp->length, &c->name)) {
        goto done;
      }
    }
    else {
      goto done;
    }
    if (slash < a.n && (slash + 2 != a.n)) {
      goto done;
    }
    c->space = space;
    c->components = zalloc(cb, ncomp * sizeof *c->components);
    if (!c->components) {
      goto done;
    }
    c->component_count = ncomp;
    for (size_t i = 0; i < ncomp; i++) {
      const int r = read_comp(cb, a.v[first + 1 + i], SLOT_NP, relative ? &sym : NULL, &c->components[i]);
      if (r != 1) {
        goto done;
      }
    }
    if (slash < a.n) {
      if (read_alpha(cb, a.v[slash + 1], true, relative ? &sym : NULL, false, relative, c) != 1) {
        goto done;
      }
    }
    ok = true;
  }
  else if (f == F_CMYK) {
    /* Color 5, device-cmyk(): `<legacy-device-cmyk-syntax> = device-cmyk( <number>#{4} )` (numbers only, commas, no alpha) and
       `<modern-device-cmyk-syntax> = device-cmyk( <cmyk-component>{4} [ / [ <alpha-value> | none ] ]? )` with
       `<cmyk-component> = <number> | <percentage> | none`; there is no relative form ("There is no relative device-cmyk() syntax"). */
    c = new_color(cb, GTEXT_CSS_COLOR_DEVICE_CMYK, v);
    if (!c) {
      goto done;
    }
    c->space = GTEXT_CSS_SPACE_CUSTOM;
    const bool commas = a.n == 7 || (a.n > 1 && is_tok(a.v[1], GTEXT_CSS_TOKEN_COMMA));
    if (commas) {
      if (a.n != 7 || !is_tok(a.v[1], GTEXT_CSS_TOKEN_COMMA) || !is_tok(a.v[3], GTEXT_CSS_TOKEN_COMMA) || !is_tok(a.v[5], GTEXT_CSS_TOKEN_COMMA)) {
        goto done;
      }
      c->legacy = true;
      c->components = zalloc(cb, 4 * sizeof *c->components);
      if (!c->components) {
        goto done;
      }
      c->component_count = 4;
      for (size_t i = 0; i < 4; i++) {
        slot_t s = {1, 0, 0, 0};
        if (read_comp(cb, a.v[i * 2], s, NULL, &c->components[i]) != 1) {
          goto done;
        }
      }
      ok = true;
    }
    else {
      const size_t rest = a.n;
      if (rest != 4 && !(rest == 6 && is_slash(a.v[4]))) {
        goto done;
      }
      c->components = zalloc(cb, 4 * sizeof *c->components);
      if (!c->components) {
        goto done;
      }
      c->component_count = 4;
      for (size_t i = 0; i < 4; i++) {
        if (read_comp(cb, a.v[i], SLOT_NP, NULL, &c->components[i]) != 1) {
          goto done;
        }
      }
      if (rest == 6 && read_alpha(cb, a.v[5], true, NULL, false, false, c) != 1) {
        goto done;
      }
      ok = true;
    }
  }
  else if (f == F_ALPHA) {
    c = new_color(cb, GTEXT_CSS_COLOR_ALPHA, v);
    if (!c) {
      goto done;
    }
    if (a.n != 4 || !is_ident(a.v[0], "from") || !is_slash(a.v[2])) {
      goto done;
    }
    c->origin = take_kid(kids, a.v[1]);
    const char * names[] = {"alpha"};
    const symbols_t sym = {names, 1};
    if (!c->origin || read_alpha(cb, a.v[3], true, &sym, false, true, c) != 1) {
      goto done;
    }
    ok = true;
  }
  else if (f == F_CONTRAST) {
    c = new_color(cb, GTEXT_CSS_COLOR_CONTRAST, v);
    if (!c) {
      goto done;
    }
    if (a.n != 1) {
      goto done;
    }
    c->origin = take_kid(kids, a.v[0]);
    ok = c->origin != NULL;
  }
  else if (f == F_LIGHT_DARK) {
    c = new_color(cb, GTEXT_CSS_COLOR_LIGHT_DARK, v);
    if (!c) {
      goto done;
    }
    if (a.n != 3 || !is_tok(a.v[1], GTEXT_CSS_TOKEN_COMMA) || kids->n != 2) {
      goto done;
    }
    const color_t ** items = zalloc(cb, 2 * sizeof *items);
    if (!items) {
      goto done;
    }
    items[0] = take_kid(kids, a.v[0]);
    items[1] = take_kid(kids, a.v[2]);
    c->items = (const color_t * const *)items;
    c->item_count = 2;
    ok = items[0] && items[1];
  }
  else if (f == F_MIX) {
    c = new_color(cb, GTEXT_CSS_COLOR_MIX, v);
    if (!c) {
      goto done;
    }
    c->mix_space = GTEXT_CSS_SPACE_OKLAB;
    size_t from = 0;
    if (a.n > 0 && is_ident(a.v[0], "in")) {
      /* in <space> [ <hue-method> hue ]? , */
      if (a.n < 3 || a.v[1]->kind != GTEXT_CSS_VALUE_TOKEN || a.v[1]->type != GTEXT_CSS_TOKEN_IDENT) {
        goto done;
      }
      GTEXT_CSS_Color_Space sp = GTEXT_CSS_SPACE_CUSTOM;
      const GTEXT_CSS_String * sn = &a.v[1]->value;
      if (space_of(sn, &sp)) {
        c->mix_space = sp;
      }
      else if (is_dashed(sn)) {
        c->mix_space = GTEXT_CSS_SPACE_CUSTOM;
        if (!copy_string(cb, sn->data, sn->length, &c->mix_custom)) {
          goto done;
        }
      }
      else {
        goto done;
      }
      c->mix_has_space = true;
      from = 2;
      const bool polar = sp == GTEXT_CSS_SPACE_HSL || sp == GTEXT_CSS_SPACE_HWB || sp == GTEXT_CSS_SPACE_LCH || sp == GTEXT_CSS_SPACE_OKLCH;
      if (from < a.n && !is_tok(a.v[from], GTEXT_CSS_TOKEN_COMMA)) {
        if (!polar || c->mix_space == GTEXT_CSS_SPACE_CUSTOM || from + 1 >= a.n || !is_ident(a.v[from + 1], "hue")) {
          goto done;
        }
        const GTEXT_CSS_String * hm = &a.v[from]->value;
        if (a.v[from]->kind != GTEXT_CSS_VALUE_TOKEN || a.v[from]->type != GTEXT_CSS_TOKEN_IDENT) {
          goto done;
        }
        if (name_is(hm, "shorter")) {
          c->hue_method = GTEXT_CSS_HUE_SHORTER;
        }
        else if (name_is(hm, "longer")) {
          c->hue_method = GTEXT_CSS_HUE_LONGER;
        }
        else if (name_is(hm, "increasing")) {
          c->hue_method = GTEXT_CSS_HUE_INCREASING;
        }
        else if (name_is(hm, "decreasing")) {
          c->hue_method = GTEXT_CSS_HUE_DECREASING;
        }
        else {
          goto done;
        }
        from += 2;
      }
      if (from >= a.n || !is_tok(a.v[from], GTEXT_CSS_TOKEN_COMMA)) {
        goto done;
      }
      from++;
    }
    size_t * starts = NULL, * ends = NULL;
    const size_t groups = split_commas(cb, &a, from, &starts, &ends);
    if (groups == (size_t)-1) {
      goto done;
    }
    if (groups != kids->n) {
      gtext_allocator_free(&cb->alloc, starts);
      goto done;
    }
    const color_t ** items = zalloc(cb, groups * sizeof *items);
    comp_t * pcts = zalloc(cb, groups * sizeof *pcts);
    bool good = items && pcts;
    for (size_t g = 0; good && g < groups; g++) {
      const size_t len = ends[g] - starts[g];
      const cv_t * colorv = NULL;
      const cv_t * pctv = NULL;
      if (len == 1) {
        colorv = a.v[starts[g]];
      }
      else if (len == 2) {
        if (looks_like_percentage(a.v[starts[g]])) {
          pctv = a.v[starts[g]];
          colorv = a.v[starts[g] + 1];
        }
        else {
          colorv = a.v[starts[g]];
          pctv = a.v[starts[g] + 1];
        }
        if (!looks_like_percentage(pctv)) {
          good = false;
          break;
        }
      }
      else {
        good = false;
        break;
      }
      items[g] = take_kid(kids, colorv);
      if (!items[g]) {
        good = false;
        break;
      }
      if (pctv) {
        slot_t s = {0, 1, 0, 0};
        if (read_comp(cb, pctv, s, NULL, &pcts[g]) != 1) {
          good = false;
          break;
        }
        if (pcts[g].kind == GTEXT_CSS_COMP_PERCENT && (pcts[g].value < 0.0 || pcts[g].value > 100.0)) {
          good = false;
          break;
        }
      }
      else {
        pcts[g].kind = GTEXT_CSS_COMP_NONE;
      }
    }
    gtext_allocator_free(&cb->alloc, starts);
    if (!good) {
      goto done;
    }
    c->items = (const color_t * const *)items;
    c->item_count = groups;
    c->percents = pcts;
    ok = true;
  }
done:
  free_args(cb, &a);
  if (!ok || cb->status != GTEXT_CSS_OK) {
    return NULL;
  }
  c->span = v->span;
  note_flags(c);
  return c;
}

static color_t * build_leaf(cb_t * cb, const cv_t * v) {
  color_t * c = NULL;
  if (v->kind != GTEXT_CSS_VALUE_TOKEN) {
    return NULL;
  }
  if (v->type == GTEXT_CSS_TOKEN_HASH) {
    const char * s = v->value.data;
    const size_t n = v->value.length;
    if (n != 3 && n != 4 && n != 6 && n != 8) {
      return NULL;
    }
    unsigned d[8];
    for (size_t i = 0; i < n; i++) {
      const char ch = s[i];
      if (ch >= '0' && ch <= '9') {
        d[i] = (unsigned)(ch - '0');
      }
      else if (ch >= 'a' && ch <= 'f') {
        d[i] = (unsigned)(ch - 'a' + 10);
      }
      else if (ch >= 'A' && ch <= 'F') {
        d[i] = (unsigned)(ch - 'A' + 10);
      }
      else {
        return NULL;
      }
    }
    c = new_color(cb, GTEXT_CSS_COLOR_HEX, v);
    if (!c) {
      return NULL;
    }
    c->space = GTEXT_CSS_SPACE_SRGB;
    c->components = zalloc(cb, 3 * sizeof *c->components);
    if (!c->components) {
      return NULL;
    }
    c->component_count = 3;
    const bool short_form = n <= 4;
    for (size_t i = 0; i < 3; i++) {
      const unsigned x = short_form ? d[i] * 17 : d[i * 2] * 16 + d[i * 2 + 1];
      c->components[i].kind = GTEXT_CSS_COMP_NUMBER;
      c->components[i].value = (double)x;
    }
    if (n == 4 || n == 8) {
      const unsigned x = short_form ? d[3] * 17 : d[6] * 16 + d[7];
      c->alpha.kind = GTEXT_CSS_COMP_NUMBER;
      c->alpha.value = (double)x / 255.0;
      c->alpha_written = true;
    }
    return c;
  }
  if (v->type != GTEXT_CSS_TOKEN_IDENT) {
    return NULL;
  }
  c = new_color(cb, GTEXT_CSS_COLOR_KEYWORD, v);
  if (!c) {
    return NULL;
  }
  c->space = GTEXT_CSS_SPACE_SRGB;
  if (name_is(&v->value, "transparent")) {
    c->keyword = GTEXT_CSS_COLOR_KW_TRANSPARENT;
    if (!copy_string(cb, "transparent", 11, &c->name)) {
      return NULL;
    }
    c->components = zalloc(cb, 3 * sizeof *c->components);
    if (!c->components) {
      return NULL;
    }
    c->component_count = 3;
    c->alpha.value = 0.0;
    c->alpha_written = true;
    return c;
  }
  if (name_is(&v->value, "currentcolor")) {
    c->keyword = GTEXT_CSS_COLOR_KW_CURRENTCOLOR;
    return copy_string(cb, "currentcolor", 12, &c->name) ? c : NULL;
  }
  uint32_t rgb;
  const char * canon;
  if (css_color_named_lookup(v->value.data, v->value.length, &rgb, &canon)) {
    c->keyword = GTEXT_CSS_COLOR_KW_NAMED;
    if (!copy_string(cb, canon, strlen(canon), &c->name)) {
      return NULL;
    }
    c->components = zalloc(cb, 3 * sizeof *c->components);
    if (!c->components) {
      return NULL;
    }
    c->component_count = 3;
    c->components[0].value = (double)((rgb >> 16) & 0xFF);
    c->components[1].value = (double)((rgb >> 8) & 0xFF);
    c->components[2].value = (double)(rgb & 0xFF);
    return c;
  }
  char lower[40];
  const int sys = v->value.length < sizeof lower ? css_color_system_lookup(v->value.data, v->value.length, lower, sizeof lower) : 0;
  if (sys) {
    c->keyword = sys == 1 ? GTEXT_CSS_COLOR_KW_SYSTEM : GTEXT_CSS_COLOR_KW_DEPRECATED;
    return copy_string(cb, lower, v->value.length, &c->name) ? c : NULL;
  }
  return NULL;
}

/* ---- the driver: nested colors, built first ----------------------------------------------------- */

typedef struct {
  const cv_t * cv;
  cvvec kids;
  bool scanned;
  size_t next;
  color_t ** results;
} cframe;

GTEXT_CSS_Status css_color_build(css_arena * arena, const GTEXT_Allocator * scratch, const cv_t * value, size_t max_nesting,
    size_t max_math_depth, size_t max_math_nodes, GTEXT_CSS_Color ** out, bool * valid, size_t * work, size_t * nodes, GTEXT_CSS_Span * where) {
  cb_t cb;
  memset(&cb, 0, sizeof cb);
  cb.arena = arena;
  cb.alloc = *scratch;
  cb.max_nesting = max_nesting ? max_nesting : DEFAULT_MAX_NESTING;
  if (cb.max_nesting > LIMIT_MAX_NESTING) {
    cb.max_nesting = LIMIT_MAX_NESTING;
  }
  cb.max_math_depth = max_math_depth;
  cb.max_math_nodes = max_math_nodes;
  *valid = false;
  *out = NULL;
  cframe * fs = NULL;
  size_t nf = 0, capf = 0;
  color_t * root = NULL;
  bool invalid = false;
#define PUSHFRAME(v_) \
  do { \
    if (nf >= cb.max_nesting) { \
      cb_fail(&cb, GTEXT_CSS_E_LIMIT_COLOR_NESTING); \
      cb.where = (v_)->span; \
      goto done; \
    } \
    if (nf == capf) { \
      const size_t c_ = capf ? capf * 2 : 8; \
      cframe * p_ = gtext_allocator_realloc(&cb.alloc, fs, c_ * sizeof *p_); \
      if (!p_) { \
        cb_fail(&cb, GTEXT_CSS_E_OOM); \
        goto done; \
      } \
      fs = p_; \
      capf = c_; \
    } \
    memset(&fs[nf], 0, sizeof fs[nf]); \
    fs[nf].cv = (v_); \
    nf++; \
  } while (0)
  if (!css_color_is_candidate(value)) {
    goto done;
  }
  PUSHFRAME(value);
  while (nf > 0 && cb.status == GTEXT_CSS_OK && !invalid) {
    cframe * F = &fs[nf - 1];
    if (!F->scanned) {
      F->scanned = true;
      if (!scan_children(&cb, F->cv, &F->kids)) {
        goto done;
      }
      if (F->kids.n) {
        F->results = gtext_allocator_calloc(&cb.alloc, F->kids.n, sizeof *F->results);
        if (!F->results) {
          cb_fail(&cb, GTEXT_CSS_E_OOM);
          goto done;
        }
      }
    }
    if (F->next < F->kids.n) {
      const cv_t * child = F->kids.v[F->next];
      PUSHFRAME(child);
      continue;
    }
    color_t * c;
    if (F->cv->kind == GTEXT_CSS_VALUE_FUNCTION) {
      const fid f = fid_of(&F->cv->value);
      kidlist kl = {F->results, F->kids.n, 0, F->kids.v};
      c = f == F_NONE ? NULL : build_function(&cb, F->cv, f, &kl);
    }
    else {
      c = build_leaf(&cb, F->cv);
    }
    cb.work++;
    gtext_allocator_free(&cb.alloc, F->kids.v);
    gtext_allocator_free(&cb.alloc, F->results);
    nf--;
    if (!c) {
      invalid = true;
      break;
    }
    if (nf == 0) {
      root = c;
    }
    else {
      cframe * P = &fs[nf - 1];
      P->results[P->next++] = c;
    }
  }
done:
  for (size_t i = 0; i < nf; i++) {
    gtext_allocator_free(&cb.alloc, fs[i].kids.v);
    gtext_allocator_free(&cb.alloc, fs[i].results);
  }
  gtext_allocator_free(&cb.alloc, fs);
#undef PUSHFRAME
  if (work) {
    *work = cb.work;
  }
  if (nodes) {
    *nodes = cb.nodes;
  }
  if (where) {
    *where = cb.where;
  }
  if (cb.status != GTEXT_CSS_OK) {
    return cb.status;
  }
  if (root && !invalid) {
    *out = root;
    *valid = true;
  }
  return GTEXT_CSS_OK;
}

/* ---- the public calls ----------------------------------------------------------------------------- */

typedef struct {
  css_arena arena;
  GTEXT_Allocator alloc;
} color_owner;

GTEXT_CSS_Status gtext_css_color_parse(const cv_t * value, const GTEXT_CSS_Color_Options * options, bool * valid,
    GTEXT_CSS_Color ** out, GTEXT_CSS_Color_Result * result) {
  if (!value || !valid || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  *valid = false;
  *out = NULL;
  GTEXT_Allocator alloc = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
  color_owner * ow = gtext_allocator_malloc(&alloc, sizeof *ow);
  if (!ow) {
    if (result) {
      memset(result, 0, sizeof *result);
      result->status = GTEXT_CSS_E_OOM;
      result->message = gtext_css_status_string(GTEXT_CSS_E_OOM);
    }
    return GTEXT_CSS_E_OOM;
  }
  ow->alloc = alloc;
  css_arena_init(&ow->arena, &ow->alloc);
  GTEXT_CSS_Color * c = NULL;
  bool ok = false;
  size_t work = 0, nodes = 0;
  GTEXT_CSS_Span where;
  memset(&where, 0, sizeof where);
  const GTEXT_CSS_Status st = css_color_build(&ow->arena, &alloc, value, options ? options->max_nesting : 0,
      options ? options->max_math_depth : 0, options ? options->max_math_nodes : 0, &c, &ok, &work, &nodes, &where);
  if (result) {
    memset(result, 0, sizeof *result);
    result->status = st;
    result->message = gtext_css_status_string(st);
    result->work = work;
    result->nodes = nodes;
    result->where = where;
  }
  if (st != GTEXT_CSS_OK || !ok) {
    css_arena_free(&ow->arena);
    gtext_allocator_free(&alloc, ow);
    if (st == GTEXT_CSS_OK) {
      *valid = false;
      *out = NULL;
    }
    return st;
  }
  c->owner_ = ow;
  *valid = true;
  *out = c;
  return GTEXT_CSS_OK;
}

void gtext_css_color_free(GTEXT_CSS_Color * color) {
  if (!color || !color->owner_) {
    return;
  }
  color_owner * ow = color->owner_;
  GTEXT_Allocator alloc = ow->alloc;
  css_arena_free(&ow->arena);
  gtext_allocator_free(&alloc, ow);
}

/* ---- the conversions the serialization of the legacy syntaxes needs --------------------------------- */

static double min2(double a, double b) {
  return a < b ? a : b;
}

static double max2(double a, double b) {
  return a > b ? a : b;
}

/// Round to a double (see css_units.c): the conversions below give the same bits where intermediates are kept wider (x87).
static inline double rnd(double x) {
  volatile double v = x;
  return v;
}

void css_color_hsl_to_srgb(double h, double s, double l, double rgb[3]) {
  h = normalize_hue(h);
  s = rnd(s / 100.0);
  l = rnd(l / 100.0);
  static const double N[3] = {0.0, 8.0, 4.0};
  const double a = rnd(s * min2(l, rnd(1.0 - l)));
  for (int i = 0; i < 3; i++) {
    const double k = css_math_fmod_exact(rnd(N[i] + rnd(h / 30.0)), 12.0);
    const double t = max2(-1.0, min2(min2(rnd(k - 3.0), rnd(9.0 - k)), 1.0));
    rgb[i] = rnd(rnd(l - rnd(a * t)) * 255.0);
  }
}

void css_color_hwb_to_srgb(double h, double w, double b, double rgb[3]) {
  w = rnd(w / 100.0);
  b = rnd(b / 100.0);
  const double sum = rnd(w + b);
  if (sum >= 1.0) {
    const double gray = rnd(rnd(w / sum) * 255.0);
    rgb[0] = rgb[1] = rgb[2] = gray;
    return;
  }
  css_color_hsl_to_srgb(h, 100.0, 50.0, rgb);
  const double scale = rnd(rnd(1.0 - w) - b);
  for (int i = 0; i < 3; i++) {
    rgb[i] = rnd(rnd(rnd(rnd(rgb[i] / 255.0) * scale) + w) * 255.0);
  }
}
