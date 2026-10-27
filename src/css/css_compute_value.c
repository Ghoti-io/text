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
 * Computing a value by its types (css_computed.h, "by-type"): a walk over a typed value that rewrites what Values 4 says a computed value is
 * different from its specified one, and shares the rest.
 *
 * - a length is absolute (px): a font-relative length against the font-size of the element (the parent's for `font-size` itself), a viewport
 *   length against the environment, a container length by its callback; an angle is in `deg`, a time in `s`, a frequency in `Hz`, a resolution in
 *   `dppx`; a percentage is kept;
 * - a math function is rebuilt with its relative lengths made px and simplified again; when the whole is one number or one quantity it is
 *   that value, clamped to the range of its slot, and a calculation that still has a percentage in it stays a calculation;
 * - `light-dark()` is resolved, a color the library cannot compute without conversion goes to the environment's callback or stays flagged;
 * - a relative URL is resolved against the base URL (RFC 3986); a reference to the document itself (`url(#id)`) stays;
 * - lists, functions and blocks are rewritten item by item.
 *
 * Nothing recurses: the walk has an explicit stack of frames, and the rebuild of a calculation another. A value that no rule changes is not copied:
 * the result is the source set itself (retained), and a value that is changed is a new set that keeps the source alive and points into it for
 * every subtree it did not change.
 */

#include <string.h>

#include <ghoti.io/math/math.h>

#include "css_color_internal.h"
#include "css_compute_internal.h"
#include "css_substitute_internal.h"

typedef struct {
  const GTEXT_CSS_Value * src;  ///< The node being walked.
  size_t i;                     ///< The next child to walk.
  GTEXT_CSS_Value * kids;       ///< The new children, once one has changed (in the new set's arena).
  bool changed;
} cv_frame;

typedef struct {
  css_state * st;
  css_vb vb;
  const GTEXT_CSS_Value_Set * srcset;
  GTEXT_CSS_Property_Id property;
  const GTEXT_CSS_Compiled * compiled;
  bool compiled_tried;
  size_t max_list;
  bool numbers_px;                ///< A number is a length in px (CV_NUMBERS_TO_PX).
  bool math_numbers_only;         ///< Only a calculation of numbers or percentages is computed (CV_MATH_NUMBERS_ONLY): a specified value that is otherwise kept.
} cv_walk;

/* ---- helpers ------------------------------------------------------------------------------------------- */

static bool slot_is_integer(cv_walk * w, const GTEXT_CSS_Value * v) {
  if (!w->compiled_tried) {
    w->compiled_tried = true;
    GTEXT_CSS_Grammar_State gs;
    if (w->property) {
      css_registry_property_compiled(w->st->cc->registry, w->property, &gs, &w->compiled, NULL);
    }
  }
  if (!w->compiled) {
    return false;
  }
  size_t len = 0;
  const char * text = gtext_css_compiled_slot_text(w->compiled, v->slot, &len);
  return text && len >= 8 && memcmp(text, "<integer", 8) == 0;
}

static double clamp_range(double v, double lo, double hi) {
  return gtext_css_math_clamp(v, lo, hi);
}

bool css_cv_length_px(css_state * st, const GTEXT_CSS_Value * v, double * px) {
  if (!v) {
    return false;
  }
  if (v->kind == GTEXT_CSS_VAL_ZERO) {
    *px = 0.0;
    return true;
  }
  if (v->kind != GTEXT_CSS_VAL_DIMENSION || gtext_css_unit_type(v->unit) != GTEXT_CSS_UNIT_TYPE_LENGTH) {
    return false;
  }
  if (gtext_css_unit_is_absolute(v->unit)) {
    return gtext_css_unit_to_canonical(v->unit, v->number, px);
  }
  double per = 0.0;
  if (!css_st_unit_px(st, v->unit, &per)) {
    return false;
  }
  *px = v->number * per;
  return true;
}

/* ---- the leaves ------------------------------------------------------------------------------------------ */

static GTEXT_CSS_Status leaf_dimension(cv_walk * w, const GTEXT_CSS_Value * src, GTEXT_CSS_Value * out, bool * changed) {
  const GTEXT_CSS_Unit_Type type = gtext_css_unit_type(src->unit);
  GTEXT_CSS_Unit target = GTEXT_CSS_UNIT_NONE;
  double number = src->number;
  switch (type) {
  case GTEXT_CSS_UNIT_TYPE_LENGTH:
    target = GTEXT_CSS_UNIT_PX;
    if (src->unit == GTEXT_CSS_UNIT_PX) {
      return GTEXT_CSS_OK;
    }
    if (!css_cv_length_px(w->st, src, &number)) {
      return w->st->status != GTEXT_CSS_OK ? w->st->status : css_st_fail(w->st, GTEXT_CSS_E_INTERNAL, NULL, "a length has no pixel size");
    }
    break;
  case GTEXT_CSS_UNIT_TYPE_ANGLE: target = GTEXT_CSS_UNIT_DEG; break;
  case GTEXT_CSS_UNIT_TYPE_TIME: target = GTEXT_CSS_UNIT_S; break;
  case GTEXT_CSS_UNIT_TYPE_FREQUENCY: target = GTEXT_CSS_UNIT_HZ; break;
  case GTEXT_CSS_UNIT_TYPE_RESOLUTION: target = GTEXT_CSS_UNIT_DPPX; break;
  default:
    return GTEXT_CSS_OK; /* a flex value and an unknown unit stay */
  }
  if (src->unit == target) {
    return GTEXT_CSS_OK;
  }
  if (type != GTEXT_CSS_UNIT_TYPE_LENGTH && !gtext_css_unit_to_canonical(src->unit, src->number, &number)) {
    return GTEXT_CSS_OK;
  }
  *out = *src;
  out->number = number;
  out->unit = target;
  *changed = true;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status leaf_url(cv_walk * w, const GTEXT_CSS_Value * src, GTEXT_CSS_Value * out, bool * changed) {
  const GTEXT_CSS_Environment * env = w->st->env;
  if (!env->base_url || !src->text.data || src->text.length == 0) {
    return GTEXT_CSS_OK;
  }
  const size_t bl = strlen(env->base_url);
  if (env->resolve_url) {
    GTEXT_CSS_String r = {NULL, 0};
    if (env->resolve_url(env->user, env->base_url, bl, src->text.data, src->text.length, &r) && r.data) {
      *out = *src;
      out->text.data = css_vb_str(&w->vb, r.data, r.length);
      out->text.length = out->text.data ? r.length : 0;
      *changed = true;
      return w->vb.status;
    }
    return GTEXT_CSS_OK;
  }
  char * resolved = NULL;
  size_t rl = 0;
  const int rc = css_resolve_url(&w->st->cc->alloc, env->base_url, bl, src->text.data, src->text.length, &resolved, &rl);
  if (rc < 0) {
    return css_st_fail(w->st, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  if (rc == 0) {
    return GTEXT_CSS_OK;
  }
  *out = *src;
  out->text.data = css_vb_str(&w->vb, resolved, rl);
  out->text.length = out->text.data ? rl : 0;
  GTEXT_Allocator a = w->st->cc->alloc;
  gtext_allocator_free(&a, resolved);
  *changed = true;
  return w->vb.status;
}

/// Serialize a color into the arena (its declared form).
static GTEXT_CSS_Status color_text(cv_walk * w, const GTEXT_CSS_Color * c, GTEXT_CSS_String * out) {
  char * buf = NULL;
  size_t len = 0;
  GTEXT_CSS_Status st = css_color_serialize_to(c, &w->st->cc->alloc, (size_t)1 << 20, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    return css_st_fail(w->st, st, NULL, NULL);
  }
  out->data = css_vb_str(&w->vb, buf, len);
  out->length = out->data ? len : 0;
  GTEXT_Allocator a = w->st->cc->alloc;
  gtext_allocator_free(&a, buf);
  return w->vb.status;
}

static GTEXT_CSS_Status leaf_color(cv_walk * w, const GTEXT_CSS_Value * src, GTEXT_CSS_Value * out, bool * changed) {
  css_state * st = w->st;
  const GTEXT_CSS_Color * c = src->color;
  if (!c) {
    return GTEXT_CSS_OK;
  }
  bool resolved_light_dark = false;
  size_t guard = 0;
  /* light-dark() takes the element's color scheme (Color Adjust 1, 2.4: "The element color scheme affects ... the value of the light-dark() function") */
  const GTEXT_CSS_Color_Scheme scheme = css_st_used_scheme(st);
  while (c->kind == GTEXT_CSS_COLOR_LIGHT_DARK && c->item_count == 2 && c->items[0] && c->items[1]) {
    c = c->items[scheme == GTEXT_CSS_SCHEME_DARK ? 1 : 0];
    resolved_light_dark = true;
    if (++guard > 100000) {
      break;
    }
  }
  const bool needs = c->kind == GTEXT_CSS_COLOR_MIX || c->kind == GTEXT_CSS_COLOR_CONTRAST || c->kind == GTEXT_CSS_COLOR_ALPHA ||
      c->kind == GTEXT_CSS_COLOR_DEVICE_CMYK || (c->kind == GTEXT_CSS_COLOR_KEYWORD && (c->keyword == GTEXT_CSS_COLOR_KW_SYSTEM || c->keyword == GTEXT_CSS_COLOR_KW_DEPRECATED)) ||
      (c->kind == GTEXT_CSS_COLOR_FUNCTION && (c->has_unresolved || c->origin != NULL));
  if (needs && st->env->resolve_color) {
    double rgba[4];
    if (st->env->resolve_color(st->env->user, c, scheme, rgba)) {
      for (int k = 0; k < 4; k++) {
        if (!(rgba[k] - rgba[k] == 0.0)) {
          return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, "resolve_color", "the resolve_color callback gave a component that is not a finite number");
        }
      }
      for (int k = 0; k < 3; k++) {
        rgba[k] = rgba[k] < 0.0 ? 0.0 : rgba[k] > 255.0 ? 255.0 : rgba[k];   /* the documented range: 0 to 255 */
      }
      rgba[3] = rgba[3] < 0.0 ? 0.0 : rgba[3] > 1.0 ? 1.0 : rgba[3];
      char num[4][336];
      size_t nl[4];
      for (int k = 0; k < 4; k++) {
        nl[k] = css_cssom_number(rgba[k], false, num[k]);
      }
      char text[1400];
      size_t t = 0;
      memcpy(text + t, "rgb(", 4);
      t += 4;
      for (int k = 0; k < 4; k++) {
        if (k == 3) {
          memcpy(text + t, " / ", 3);
          t += 3;
        }
        else if (k > 0) {
          text[t++] = ' ';
        }
        memcpy(text + t, num[k], nl[k]);
        t += nl[k];
      }
      text[t++] = ')';
      GTEXT_CSS_Property_Options po;
      memset(&po, 0, sizeof po);
      po.allocator = &st->cc->alloc;
      GTEXT_CSS_Property_Value pv;
      memset(&pv, 0, sizeof pv);
      const GTEXT_CSS_Property_Id color_id = st->cc->pid[CSS_P_COLOR];
      GTEXT_CSS_Status ps = color_id ? gtext_css_parse_property_text(st->cc->registry, st->cc->meta[color_id - 1].name,
          st->cc->meta[color_id - 1].name_length, text, t, &po, &pv) : GTEXT_CSS_E_UNSUPPORTED;
      if (ps == GTEXT_CSS_OK && pv.outcome == GTEXT_CSS_PROP_RESULT_VALID && gtext_css_value_set_root(pv.set)) {
        const GTEXT_CSS_Value * pr = gtext_css_value_set_root(pv.set);
        *out = *src;
        out->color = pr->color;
        out->text = pr->text;
        out->flags &= ~(uint32_t)GTEXT_CSS_VFLAG_COLOR_UNRESOLVED;
        if (!css_vb_dep(&w->vb, pv.set)) {
          gtext_css_property_value_free(&pv);
          return w->vb.status;
        }
        gtext_css_property_value_free(&pv);
        *changed = true;
        return GTEXT_CSS_OK;
      }
      gtext_css_property_value_free(&pv);
      if (ps == GTEXT_CSS_E_OOM) {
        return css_st_fail(st, ps, NULL, NULL);
      }
    }
  }
  if (!resolved_light_dark && !needs) {
    return GTEXT_CSS_OK;
  }
  if (!resolved_light_dark && (src->flags & GTEXT_CSS_VFLAG_COLOR_UNRESOLVED)) {
    return GTEXT_CSS_OK;
  }
  *out = *src;
  out->color = c;
  if (resolved_light_dark) {
    GTEXT_CSS_Status ts = color_text(w, c, &out->text);
    if (ts != GTEXT_CSS_OK) {
      return ts;
    }
  }
  if (needs) {
    out->flags |= GTEXT_CSS_VFLAG_COLOR_UNRESOLVED;
  }
  *changed = true;
  return GTEXT_CSS_OK;
}

/* ---- calculations ---------------------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Math_Node * n;
  size_t i;
  size_t base;
} mframe;

typedef struct {
  mbuilder mb;
  const GTEXT_CSS_Math_Node ** res;
  size_t nres, capres;
  mframe * frames;
  size_t nframes, capframes;
  bool has_symbols;
  bool oom;
} mrebuild;

static bool mr_push_result(mrebuild * r, GTEXT_Allocator * a, const GTEXT_CSS_Math_Node * n) {
  if (r->nres == r->capres) {
    const size_t cap = r->capres ? r->capres * 2 : 32;
    if (cap > SIZE_MAX / sizeof *r->res) {
      r->oom = true;
      return false;
    }
    const GTEXT_CSS_Math_Node ** grown = gtext_allocator_realloc(a, (void *)r->res, cap * sizeof *r->res);
    if (!grown) {
      r->oom = true;
      return false;
    }
    r->res = grown;
    r->capres = cap;
  }
  r->res[r->nres++] = n;
  return true;
}

static bool mr_push_frame(mrebuild * r, GTEXT_Allocator * a, const GTEXT_CSS_Math_Node * n) {
  if (r->nframes == r->capframes) {
    const size_t cap = r->capframes ? r->capframes * 2 : 32;
    if (cap > SIZE_MAX / sizeof *r->frames) {
      r->oom = true;
      return false;
    }
    mframe * grown = gtext_allocator_realloc(a, r->frames, cap * sizeof *r->frames);
    if (!grown) {
      r->oom = true;
      return false;
    }
    r->frames = grown;
    r->capframes = cap;
  }
  r->frames[r->nframes].n = n;
  r->frames[r->nframes].i = 0;
  r->frames[r->nframes].base = r->nres;
  r->nframes++;
  return true;
}

/// One node of the calculation with its operands (already rebuilt) made again.
static const GTEXT_CSS_Math_Node * mr_node(cv_walk * w, mrebuild * r, const GTEXT_CSS_Math_Node * n, const GTEXT_CSS_Math_Node * const * args) {
  mbuilder * mb = &r->mb;
  switch (n->op) {
  case GTEXT_CSS_MATH_NUMBER:
    return mk_number(mb, n->value);
  case GTEXT_CSS_MATH_PERCENT:
    return mk_percent(mb, n->value);
  case GTEXT_CSS_MATH_DIMENSION: {
    double v = n->value;
    GTEXT_CSS_Unit unit = n->unit;
    if (!gtext_css_unit_is_absolute(unit)) {
      GTEXT_CSS_Value tmp;
      memset(&tmp, 0, sizeof tmp);
      tmp.kind = GTEXT_CSS_VAL_DIMENSION;
      tmp.number = n->value;
      tmp.unit = unit;
      double px = 0;
      if (!css_cv_length_px(w->st, &tmp, &px)) {
        css_st_fail(w->st, GTEXT_CSS_E_UNSUPPORTED, gtext_css_unit_name(unit), "a unit inside a calculation has no size in this environment");
        return NULL;
      }
      v = px;
      unit = GTEXT_CSS_UNIT_PX;
    }
    return mk_dimension(mb, v, unit);
  }
  case GTEXT_CSS_MATH_UNRESOLVED: {
    /* sibling-index() and sibling-count() are numbers the element tree answers (the callbacks of that name); a computation with no table for the tree fails
       naming the callback, and a value that is not a number is never kept */
    const bool index = n->text.length >= 13 && memcmp(n->text.data, "sibling-index", 13) == 0;
    const bool count = n->text.length >= 13 && memcmp(n->text.data, "sibling-count", 13) == 0;
    if (index || count) {
      double v = 0;
      if (css_st_sibling(w->st, index, &v) != GTEXT_CSS_OK) {
        return NULL;
      }
      return mk_number(mb, v);
    }
    r->has_symbols = true;
    return n; /* shared: it lives in the source set */
  }
  case GTEXT_CSS_MATH_RANDOM: {
    /* random(): with literal arguments of one kind it is the number its random base value picks; with anything else (a percentage against a length ...) it is kept
       for the math layer */
    const GTEXT_CSS_Math_Node * const * kids = args;
    if ((n->kid_count == 2 || n->kid_count == 3) && kids) {
      bool literal = true;
      const GTEXT_CSS_Math_Node * k0 = kids[0];
      for (size_t i = 0; i < n->kid_count; i++) {
        literal = literal && (kids[i]->op == k0->op) && (k0->op == GTEXT_CSS_MATH_NUMBER || k0->op == GTEXT_CSS_MATH_PERCENT || k0->op == GTEXT_CSS_MATH_DIMENSION);
        literal = literal && (k0->op != GTEXT_CSS_MATH_DIMENSION || gtext_css_unit_type(kids[i]->unit) == gtext_css_unit_type(k0->unit));
      }
      double v[3] = {0, 0, 0};
      for (size_t i = 0; literal && i < n->kid_count; i++) {
        v[i] = kids[i]->value;
        if (k0->op == GTEXT_CSS_MATH_DIMENSION && !gtext_css_unit_to_canonical(kids[i]->unit, kids[i]->value, &v[i])) {
          literal = false;
        }
      }
      if (literal) {
        double R = 0;
        if (!css_st_random_base(w->st, n->text.data ? n->text.data : "", n->text.length, &R)) {
          css_st_fail(w->st, GTEXT_CSS_E_INVALID, "random", "the key of a random() is not a <random-key>");
          return NULL;
        }
        const double result = css_random_pick(v[0], v[1], v[2], n->kid_count == 3, R);
        if (k0->op == GTEXT_CSS_MATH_NUMBER) {
          return mk_number(mb, result);
        }
        if (k0->op == GTEXT_CSS_MATH_PERCENT) {
          return mk_percent(mb, result);
        }
        return mk_dimension(mb, result, gtext_css_unit_canonical(k0->unit));
      }
    }
    mfn_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.key = n->text.data;
    opts.key_len = n->text.length;
    return mk_fn(mb, n->op, args, n->kid_count, &opts);
  }
  case GTEXT_CSS_MATH_SYMBOL:
    r->has_symbols = true;
    return n; /* shared: it lives in the source set */
  case GTEXT_CSS_MATH_SUM:
    return mk_sum(mb, args, n->kid_count);
  case GTEXT_CSS_MATH_PRODUCT:
    return mk_product(mb, args, n->kid_count);
  case GTEXT_CSS_MATH_NEGATE:
    return mk_negate(mb, args[0]);
  case GTEXT_CSS_MATH_INVERT:
    return mk_invert(mb, args[0]);
  default: {
    mfn_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.round = n->round;
    opts.strategy_written = n->strategy_written;
    opts.no_clamp = n->no_clamp;
    opts.key = n->text.data;
    opts.key_len = n->text.length;
    return mk_fn(mb, n->op, args, n->kid_count, &opts);
  }
  }
}

static GTEXT_CSS_Status leaf_math(cv_walk * w, const GTEXT_CSS_Value * src, GTEXT_CSS_Value * out, bool * changed) {
  css_state * st = w->st;
  GTEXT_Allocator alloc = st->cc->alloc;
  mrebuild r;
  memset(&r, 0, sizeof r);
  r.mb.arena = &w->vb.set->arena;
  r.mb.alloc = alloc;
  r.mb.max_depth = 65536;
  r.mb.max_nodes = st->cc->options.max_value_nodes;
  r.mb.status = GTEXT_CSS_OK;
  const GTEXT_CSS_Math * m = src->math;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  if (!mr_push_frame(&r, &alloc, m->root)) {
    status = GTEXT_CSS_E_OOM;
  }
  while (status == GTEXT_CSS_OK && r.nframes) {
    mframe * f = &r.frames[r.nframes - 1];
    if (f->i < f->n->kid_count) {
      const GTEXT_CSS_Math_Node * kid = f->n->kids[f->i++];
      if (!mr_push_frame(&r, &alloc, kid)) {
        status = GTEXT_CSS_E_OOM;
      }
      continue;
    }
    const GTEXT_CSS_Math_Node * n = f->n;
    const size_t base = f->base;
    const GTEXT_CSS_Math_Node * made = mr_node(w, &r, n, (const GTEXT_CSS_Math_Node * const *)(r.res ? r.res + base : NULL));
    if (!made) {
      status = st->status != GTEXT_CSS_OK ? st->status : (r.mb.status != GTEXT_CSS_OK ? r.mb.status : GTEXT_CSS_E_INTERNAL);
      break;
    }
    r.nframes--;
    r.nres = base;
    if (!mr_push_result(&r, &alloc, made)) {
      status = GTEXT_CSS_E_OOM;
    }
    st->work++;
  }
  st->work += r.mb.work; /* the nodes the builder made and looked at: the walk's own steps are counted above */
  const GTEXT_CSS_Math_Node * root = status == GTEXT_CSS_OK && r.nres == 1 ? r.res[0] : NULL;
  gtext_allocator_free(&alloc, (void *)r.res);
  gtext_allocator_free(&alloc, r.frames);
  if (status != GTEXT_CSS_OK) {
    if (r.mb.status != GTEXT_CSS_OK) {
      status = r.mb.status;
    }
    return css_st_fail(st, status, NULL, NULL);
  }
  if (!root) {
    return css_st_fail(st, GTEXT_CSS_E_INTERNAL, NULL, "a calculation was not rebuilt");
  }
  /* one number or one quantity: the plain value, clamped to the range of the slot */
  if (!r.has_symbols && (root->op == GTEXT_CSS_MATH_NUMBER || root->op == GTEXT_CSS_MATH_PERCENT || root->op == GTEXT_CSS_MATH_DIMENSION)) {
    *out = *src;
    out->flags &= ~(uint32_t)(GTEXT_CSS_VFLAG_MATH | GTEXT_CSS_VFLAG_MATH_PERCENT);
    out->math = NULL;
    out->text.data = NULL;
    out->text.length = 0;
    out->children = NULL;
    out->child_count = 0;
    double v = clamp_range(root->value, src->range_lo, src->range_hi);
    if (root->op == GTEXT_CSS_MATH_NUMBER) {
      if (slot_is_integer(w, src)) {
        css_v_integer(out, (double)0);
        /* round half toward +infinity (Values 4, 10.2: "rounded to the nearest integer") */
        double t = v + 0.5;
        const double f = gmath_floor(t);
        out->number = clamp_range(f, src->range_lo, src->range_hi);
      }
      else {
        css_v_number(out, v);
      }
    }
    else if (root->op == GTEXT_CSS_MATH_PERCENT) {
      css_v_percentage(out, v);
    }
    else {
      css_v_dimension(out, v, root->unit);
    }
    out->slot = src->slot;
    *changed = true;
    return GTEXT_CSS_OK;
  }
  /* still a calculation (a percentage, or a keyword that is not known yet) */
  GTEXT_CSS_Math * nm = css_arena_alloc(&w->vb.set->arena, sizeof *nm);
  if (!nm) {
    return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  memset(nm, 0, sizeof *nm);
  nm->root = root;
  nm->type = root->type;
  nm->category = css_math_type_category(&root->type);
  nm->has_percent = m->has_percent;
  nm->has_symbols = r.has_symbols;
  nm->node_count = r.mb.nodes;
  char * text = NULL;
  size_t tl = 0;
  status = css_math_serialize_to(nm, &alloc, (size_t)1 << 24, &text, &tl);
  if (status != GTEXT_CSS_OK) {
    return css_st_fail(st, status, NULL, NULL);
  }
  *out = *src;
  out->math = nm;
  out->text.data = css_vb_str(&w->vb, text, tl);
  out->text.length = out->text.data ? tl : 0;
  gtext_allocator_free(&alloc, text);
  *changed = true;
  return w->vb.status;
}

/* ---- the walk -------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status leaf_base(cv_walk * w, const GTEXT_CSS_Value * src, GTEXT_CSS_Value * out, bool * changed) {
  if ((src->flags & GTEXT_CSS_VFLAG_MATH) && src->math) {
    if (w->math_numbers_only && src->math->category != GTEXT_CSS_MATH_CAT_NUMBER && src->math->category != GTEXT_CSS_MATH_CAT_PERCENTAGE) {
      return GTEXT_CSS_OK;
    }
    return leaf_math(w, src, out, changed);
  }
  if (w->math_numbers_only) {
    return GTEXT_CSS_OK;
  }
  switch (src->kind) {
  case GTEXT_CSS_VAL_DIMENSION:
    return leaf_dimension(w, src, out, changed);
  case GTEXT_CSS_VAL_COLOR:
    return leaf_color(w, src, out, changed);
  case GTEXT_CSS_VAL_URL:
    return leaf_url(w, src, out, changed);
  default:
    return GTEXT_CSS_OK;
  }
}

static GTEXT_CSS_Status leaf(cv_walk * w, const GTEXT_CSS_Value * src, GTEXT_CSS_Value * out, bool * changed) {
  GTEXT_CSS_Status s = leaf_base(w, src, out, changed);
  if (s != GTEXT_CSS_OK || !w->numbers_px) {
    return s;
  }
  const GTEXT_CSS_Value * now = *changed ? out : src;
  if ((now->kind == GTEXT_CSS_VAL_NUMBER || now->kind == GTEXT_CSS_VAL_INTEGER) && !((now->flags & GTEXT_CSS_VFLAG_MATH) && now->math)) {
    GTEXT_CSS_Value v = *now;
    v.kind = GTEXT_CSS_VAL_DIMENSION;
    v.unit = GTEXT_CSS_UNIT_PX;
    v.interp = GTEXT_CSS_INTERP_LENGTH;
    *out = v;
    *changed = true;
  }
  return GTEXT_CSS_OK;
}

static bool has_children(const GTEXT_CSS_Value * v) {
  if ((v->flags & GTEXT_CSS_VFLAG_MATH) && v->math) {
    return false;
  }
  return (v->kind == GTEXT_CSS_VAL_LIST || v->kind == GTEXT_CSS_VAL_FUNCTION || v->kind == GTEXT_CSS_VAL_BLOCK) && v->child_count > 0;
}

GTEXT_CSS_Status css_cv_by_type(css_state * st, const GTEXT_CSS_Value_Set * srcset, GTEXT_CSS_Property_Id property, GTEXT_CSS_Value_Set ** out) {
  return css_cv_by_type_ex(st, srcset, property, 0, out);
}

GTEXT_CSS_Status css_cv_by_type_ex(css_state * st, const GTEXT_CSS_Value_Set * srcset, GTEXT_CSS_Property_Id property, unsigned flags,
    GTEXT_CSS_Value_Set ** out) {
  *out = NULL;
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(srcset);
  if (!root) {
    *out = gtext_css_value_set_retain(srcset);
    return GTEXT_CSS_OK;
  }
  cv_walk w;
  memset(&w, 0, sizeof w);
  w.st = st;
  w.srcset = srcset;
  w.property = property;
  w.max_list = st->cc->options.max_list_items;
  w.numbers_px = (flags & CV_NUMBERS_TO_PX) != 0;
  w.math_numbers_only = (flags & CV_MATH_NUMBERS_ONLY) != 0;
  GTEXT_Allocator alloc = st->cc->alloc;
  if (!css_vb_begin(&w.vb, &alloc, st->cc->options.max_value_nodes)) {
    return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  cv_frame * frames = NULL;
  size_t nframes = 0, capframes = 0;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  GTEXT_CSS_Value result;
  bool changed = false;
  memset(&result, 0, sizeof result);

  /* the root: a leaf is done at once; a node with children gets a frame */
  if (!has_children(root)) {
    status = leaf(&w, root, &result, &changed);
  }
  else {
    capframes = 16;
    frames = gtext_allocator_malloc(&alloc, capframes * sizeof *frames);
    if (!frames) {
      status = GTEXT_CSS_E_OOM;
    }
    else {
      if (root->child_count > w.max_list) {
        status = GTEXT_CSS_E_LIMIT_LIST;
      }
      frames[0] = (cv_frame){root, 0, NULL, false};
      nframes = 1;
    }
    while (status == GTEXT_CSS_OK && nframes) {
      cv_frame * f = &frames[nframes - 1];
      if (f->i < f->src->child_count) {
        const GTEXT_CSS_Value * kid = &f->src->children[f->i];
        GTEXT_CSS_Value kres;
        bool kchanged = false;
        memset(&kres, 0, sizeof kres);
        if (has_children(kid)) {
          if (kid->child_count > w.max_list) {
            status = GTEXT_CSS_E_LIMIT_LIST;
            break;
          }
          if (nframes == capframes) {
            if (capframes > SIZE_MAX / 2 / sizeof *frames) {
              status = GTEXT_CSS_E_OOM;
              break;
            }
            cv_frame * grown = gtext_allocator_realloc(&alloc, frames, capframes * 2 * sizeof *frames);
            if (!grown) {
              status = GTEXT_CSS_E_OOM;
              break;
            }
            frames = grown;
            capframes *= 2;
            f = &frames[nframes - 1];
          }
          frames[nframes++] = (cv_frame){kid, 0, NULL, false};
          continue;
        }
        status = leaf(&w, kid, &kres, &kchanged);
        if (status != GTEXT_CSS_OK) {
          break;
        }
        st->work++;
        if (kchanged) {
          if (!f->kids) {
            f->kids = css_vb_values(&w.vb, f->src->child_count);
            if (!f->kids) {
              status = w.vb.status;
              break;
            }
            memcpy(f->kids, f->src->children, f->src->child_count * sizeof *f->kids);
          }
          f->kids[f->i] = kres;
          f->changed = true;
        }
        f->i++;
        continue;
      }
      /* a node whose children are all done */
      GTEXT_CSS_Value done = *f->src;
      const bool node_changed = f->changed;
      if (node_changed) {
        done.children = f->kids;
      }
      nframes--;
      if (nframes == 0) {
        result = done;
        changed = node_changed;
        break;
      }
      cv_frame * parent = &frames[nframes - 1];
      if (node_changed) {
        if (!parent->kids) {
          parent->kids = css_vb_values(&w.vb, parent->src->child_count);
          if (!parent->kids) {
            status = w.vb.status;
            break;
          }
          memcpy(parent->kids, parent->src->children, parent->src->child_count * sizeof *parent->kids);
        }
        parent->kids[parent->i] = done;
        parent->changed = true;
      }
      parent->i++;
      st->work++;
    }
  }
  gtext_allocator_free(&alloc, frames);
  if (status == GTEXT_CSS_OK && w.vb.status != GTEXT_CSS_OK) {
    status = w.vb.status;
  }
  if (status != GTEXT_CSS_OK) {
    css_vb_abort(&w.vb);
    if (st->status == GTEXT_CSS_OK) {
      css_st_fail(st, status, NULL, status == GTEXT_CSS_E_LIMIT_LIST ? "a list has more items than max_list_items" : NULL);
    }
    return st->status;
  }
  if (!changed) {
    css_vb_abort(&w.vb);
    *out = gtext_css_value_set_retain(srcset);
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * rv = css_vb_value(&w.vb);
  if (!rv || !css_vb_dep(&w.vb, srcset)) {
    status = w.vb.status ? w.vb.status : GTEXT_CSS_E_OOM;
    css_vb_abort(&w.vb);
    return css_st_fail(st, status, NULL, NULL);
  }
  *rv = result;
  GTEXT_CSS_Value_Set * set = css_vb_finish(&w.vb, rv);
  if (!set) {
    return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  *out = set;
  return GTEXT_CSS_OK;
}
