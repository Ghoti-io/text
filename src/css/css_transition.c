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
 * CSS Transitions 1, "Starting of transitions" (css_transition.h): the set of running and completed transitions of one element, and the algorithm that a style change event runs on it.
 * Every sentence the code follows is quoted where it is followed. The sampling of a running transition into a style is css_transition_sample() (css_anim_sample.c), which shares the
 * combination of two values with the animations.
 */

#include <ghoti.io/text/css.h>

#include <stdlib.h>
#include <string.h>

#include "css_anim_internal.h"
#include "css_compute_internal.h"

/* ---- the set -------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSS_Property_Id prop;
  char * start; size_t ls;
  char * end; size_t le;
  char * rasv; size_t lr;     ///< the reversing-adjusted start value
  char * timing; size_t lt;   ///< the matching timing function, serialized
  double start_time, end_time, rsf;
  bool allow_discrete;
} tr_t;

typedef struct {
  GTEXT_CSS_Property_Id prop;
  char * end; size_t le;
} done_t;

struct GTEXT_CSS_Transition_Set {
  GTEXT_Allocator alloc;
  tr_t * run;
  size_t nrun, caprun;
  done_t * done;
  size_t ndone, capdone;
  uint64_t version;
};

static char * dupn(const GTEXT_Allocator * a, const char * s, size_t n) {
  char * d = gtext_allocator_malloc((GTEXT_Allocator *)a, n + 1);
  if (d) {
    if (n) {
      memcpy(d, s, n);
    }
    d[n] = '\0';
  }
  return d;
}

static void tr_free(const GTEXT_Allocator * a, tr_t * t) {
  gtext_allocator_free((GTEXT_Allocator *)a, t->start);
  gtext_allocator_free((GTEXT_Allocator *)a, t->end);
  gtext_allocator_free((GTEXT_Allocator *)a, t->rasv);
  gtext_allocator_free((GTEXT_Allocator *)a, t->timing);
  memset(t, 0, sizeof *t);
}

static void done_free(const GTEXT_Allocator * a, done_t * d) {
  gtext_allocator_free((GTEXT_Allocator *)a, d->end);
  memset(d, 0, sizeof *d);
}

static void contents_free(GTEXT_CSS_Transition_Set * s) {
  for (size_t i = 0; i < s->nrun; i++) {
    tr_free(&s->alloc, &s->run[i]);
  }
  for (size_t i = 0; i < s->ndone; i++) {
    done_free(&s->alloc, &s->done[i]);
  }
  gtext_allocator_free(&s->alloc, s->run);
  gtext_allocator_free(&s->alloc, s->done);
  s->run = NULL;
  s->done = NULL;
  s->nrun = s->caprun = s->ndone = s->capdone = 0;
}

GTEXT_CSS_Status gtext_css_transition_set_new(const GTEXT_Allocator * allocator, GTEXT_CSS_Transition_Set ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  GTEXT_Allocator a = allocator ? *allocator : *gtext_allocator_default();
  GTEXT_CSS_Transition_Set * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  s->alloc = a;
  *out = s;
  return GTEXT_CSS_OK;
}

void gtext_css_transition_set_free(GTEXT_CSS_Transition_Set * set) {
  if (!set) {
    return;
  }
  contents_free(set);
  GTEXT_Allocator a = set->alloc;
  gtext_allocator_free(&a, set);
}

void gtext_css_transition_set_clear(GTEXT_CSS_Transition_Set * set) {
  if (set && (set->nrun || set->ndone)) {
    contents_free(set);
    set->version++;
  }
}

uint64_t gtext_css_transition_set_version(const GTEXT_CSS_Transition_Set * set) {
  return set ? set->version : 0;
}

size_t gtext_css_transition_set_running_count(const GTEXT_CSS_Transition_Set * set) {
  return set ? set->nrun : 0;
}

bool gtext_css_transition_set_running_at(const GTEXT_CSS_Transition_Set * set, size_t index, GTEXT_CSS_Transition_Info * out) {
  if (!set || !out || index >= set->nrun) {
    return false;
  }
  const tr_t * t = &set->run[index];
  out->property = t->prop;
  out->start_value = (GTEXT_CSS_String){t->start, t->ls};
  out->end_value = (GTEXT_CSS_String){t->end, t->le};
  out->reversing_adjusted_start_value = (GTEXT_CSS_String){t->rasv, t->lr};
  out->start_time_ms = t->start_time;
  out->end_time_ms = t->end_time;
  out->reversing_shortening_factor = t->rsf;
  out->timing_function = (GTEXT_CSS_String){t->timing, t->lt};
  out->allow_discrete = t->allow_discrete;
  return true;
}

size_t gtext_css_transition_set_completed_count(const GTEXT_CSS_Transition_Set * set) {
  return set ? set->ndone : 0;
}

bool gtext_css_transition_set_completed_at(const GTEXT_CSS_Transition_Set * set, size_t index, GTEXT_CSS_Property_Id * property, GTEXT_CSS_String * end_value) {
  if (!set || index >= set->ndone) {
    return false;
  }
  if (property) {
    *property = set->done[index].prop;
  }
  if (end_value) {
    *end_value = (GTEXT_CSS_String){set->done[index].end, set->done[index].le};
  }
  return true;
}

/// A deep copy of a set, for the work of one update (the set is replaced by it only if the whole update succeeded).
static GTEXT_CSS_Status set_copy(const GTEXT_CSS_Transition_Set * src, GTEXT_CSS_Transition_Set ** out) {
  GTEXT_CSS_Transition_Set * c = NULL;
  GTEXT_CSS_Status st = gtext_css_transition_set_new(&src->alloc, &c);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  c->version = src->version;
  if (src->nrun) {
    c->run = gtext_allocator_calloc(&c->alloc, src->nrun, sizeof *c->run);
    if (!c->run) {
      gtext_css_transition_set_free(c);
      return GTEXT_CSS_E_OOM;
    }
    c->caprun = src->nrun;
  }
  if (src->ndone) {
    c->done = gtext_allocator_calloc(&c->alloc, src->ndone, sizeof *c->done);
    if (!c->done) {
      gtext_css_transition_set_free(c);
      return GTEXT_CSS_E_OOM;
    }
    c->capdone = src->ndone;
  }
  for (size_t i = 0; i < src->nrun; i++) {
    const tr_t * a = &src->run[i];
    tr_t * b = &c->run[i];
    *b = *a;
    b->start = dupn(&c->alloc, a->start, a->ls);
    b->end = dupn(&c->alloc, a->end, a->le);
    b->rasv = dupn(&c->alloc, a->rasv, a->lr);
    b->timing = dupn(&c->alloc, a->timing, a->lt);
    c->nrun = i + 1;
    if (!b->start || !b->end || !b->rasv || !b->timing) {
      gtext_css_transition_set_free(c);
      return GTEXT_CSS_E_OOM;
    }
  }
  for (size_t i = 0; i < src->ndone; i++) {
    c->done[i].prop = src->done[i].prop;
    c->done[i].le = src->done[i].le;
    c->done[i].end = dupn(&c->alloc, src->done[i].end, src->done[i].le);
    c->ndone = i + 1;
    if (!c->done[i].end) {
      gtext_css_transition_set_free(c);
      return GTEXT_CSS_E_OOM;
    }
  }
  *out = c;
  return GTEXT_CSS_OK;
}

/// Replace the contents of `dst` by those of `src` (`src` is left empty and freed by the caller).
static void set_take(GTEXT_CSS_Transition_Set * dst, GTEXT_CSS_Transition_Set * src) {
  contents_free(dst);
  dst->run = src->run;
  dst->nrun = src->nrun;
  dst->caprun = src->caprun;
  dst->done = src->done;
  dst->ndone = src->ndone;
  dst->capdone = src->capdone;
  src->run = NULL;
  src->done = NULL;
  src->nrun = src->caprun = src->ndone = src->capdone = 0;
}

static GTEXT_CSS_Status run_push(GTEXT_CSS_Transition_Set * s, const tr_t * t) {
  if (s->nrun == s->caprun) {
    const size_t cap = s->caprun ? s->caprun * 2 : 4;
    tr_t * bigger = gtext_allocator_realloc(&s->alloc, s->run, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_CSS_E_OOM;
    }
    s->run = bigger;
    s->caprun = cap;
  }
  s->run[s->nrun++] = *t;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status done_push(GTEXT_CSS_Transition_Set * s, GTEXT_CSS_Property_Id prop, char * end, size_t le) {
  if (s->ndone == s->capdone) {
    const size_t cap = s->capdone ? s->capdone * 2 : 4;
    done_t * bigger = gtext_allocator_realloc(&s->alloc, s->done, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_CSS_E_OOM;
    }
    s->done = bigger;
    s->capdone = cap;
  }
  s->done[s->ndone].prop = prop;
  s->done[s->ndone].end = end;
  s->done[s->ndone].le = le;
  s->ndone++;
  return GTEXT_CSS_OK;
}

static void run_remove(GTEXT_CSS_Transition_Set * s, size_t i) {
  tr_free(&s->alloc, &s->run[i]);
  memmove(&s->run[i], &s->run[i + 1], (s->nrun - i - 1) * sizeof *s->run);
  s->nrun--;
}

static void done_remove(GTEXT_CSS_Transition_Set * s, size_t i) {
  done_free(&s->alloc, &s->done[i]);
  memmove(&s->done[i], &s->done[i + 1], (s->ndone - i - 1) * sizeof *s->done);
  s->ndone--;
}

static size_t run_find(const GTEXT_CSS_Transition_Set * s, GTEXT_CSS_Property_Id p) {
  for (size_t i = 0; i < s->nrun; i++) {
    if (s->run[i].prop == p) {
      return i;
    }
  }
  return (size_t)-1;
}

static size_t done_find(const GTEXT_CSS_Transition_Set * s, GTEXT_CSS_Property_Id p) {
  for (size_t i = 0; i < s->ndone; i++) {
    if (s->done[i].prop == p) {
      return i;
    }
  }
  return (size_t)-1;
}

static bool same(const char * a, size_t al, const char * b, size_t bl) {
  return al == bl && (al == 0 || memcmp(a, b, al) == 0);
}

/* ---- the current value --------------------------------------------------------------------------------------------------------------------------- */

/// The current value of a transition at a time: "If the current time is at or before the start time of the transition (that is, during the delay phase of the transition), the current
/// value is a specified style that will compute to the start value of the transition. If the current time is after the start time ... the result of interpolating the property using ...
/// p: the output of the timing function for input (current time - start time) / (end time - start time)".
static GTEXT_CSS_Status tr_current(const GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Computed_Style * style, const tr_t * t, double time_ms, const GTEXT_Allocator * alloc, char ** text,
    size_t * length) {
  const GTEXT_Allocator * a = alloc ? alloc : &cc->alloc;
  if (time_ms <= t->start_time || t->end_time <= t->start_time) {
    if (time_ms < t->end_time || t->end_time > t->start_time) {
      *text = dupn(a, t->start, t->ls);
      *length = t->ls;
    }
    else {
      *text = dupn(a, t->end, t->le);
      *length = t->le;
    }
    return *text ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (time_ms >= t->end_time) {
    *text = dupn(a, t->end, t->le);
    *length = t->le;
    return *text ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Easing * easing = NULL;
  GTEXT_CSS_Status st = css_easing_from_text(gtext_css_compute_context_registry(cc), a, t->timing, t->lt, &easing);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const double p = gtext_css_easing_output(easing, (time_ms - t->start_time) / (t->end_time - t->start_time), false);
  gtext_css_easing_free(easing);
  bool discrete = false;
  return css_blend_values_text(cc, style, t->prop, t->start, t->ls, t->end, t->le, p, a, text, length, &discrete);
}

GTEXT_CSS_Status gtext_css_transition_current_value(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Computed_Style * style, const GTEXT_CSS_Transition_Set * set, size_t index,
    double time_ms, const GTEXT_Allocator * allocator, char ** text, size_t * length) {
  if (!context || !set || !text || !length || index >= set->nrun) {
    return GTEXT_CSS_E_INVALID;
  }
  return tr_current(context, style, &set->run[index], time_ms, allocator, text, length);
}

/* ---- the lists of the after-change style --------------------------------------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Value * items;
  size_t n;
} list_t;

static bool read_list(const GTEXT_CSS_Computed_Style * style, const char * name, list_t * out) {
  const GTEXT_CSS_Registry * r = gtext_css_compute_context_registry(style->cc);
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(r, name, strlen(name));
  out->items = NULL;
  out->n = 0;
  if (!id) {
    return false;
  }
  const GTEXT_CSS_Value * root = gtext_css_computed_root(style, id);
  if (!root) {
    return false;
  }
  if (root->kind == GTEXT_CSS_VAL_LIST && root->separator == GTEXT_CSS_SEP_COMMA) {
    out->items = root->children;
    out->n = root->child_count;
  }
  else {
    out->items = root;
    out->n = 1;
  }
  return out->n > 0;
}

static bool ieq(const char * a, size_t al, const char * b) {
  const size_t bl = strlen(b);
  if (al != bl) {
    return false;
  }
  for (size_t i = 0; i < al; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = (char)(x + 32);
    }
    if (y >= 'A' && y <= 'Z') {
      y = (char)(y + 32);
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

static bool is_kw(const GTEXT_CSS_Value * v, const char * w) {
  return v->kind == GTEXT_CSS_VAL_KEYWORD && ieq(v->text.data, v->text.length, w);
}

static bool time_of(const GTEXT_CSS_Value * v, double * ms) {
  double s = 0;
  if (v->kind == GTEXT_CSS_VAL_DIMENSION && gtext_css_unit_to_canonical(v->unit, v->number, &s) && gtext_css_unit_type(v->unit) == GTEXT_CSS_UNIT_TYPE_TIME) {
    *ms = s * 1000.0;
    return true;
  }
  if (v->kind == GTEXT_CSS_VAL_ZERO) {
    *ms = 0;
    return true;
  }
  return false;
}

/// Whether a longhand `p` is covered by the shorthand `sh` (at any depth).
static bool covers(const GTEXT_CSS_Registry * r, GTEXT_CSS_Property_Id sh, GTEXT_CSS_Property_Id p, int depth) {
  if (depth > 4) {
    return false;
  }
  for (size_t i = 0;; i++) {
    const GTEXT_CSS_Property_Id l = gtext_css_registry_longhand(r, sh, i);
    if (!l) {
      return false;
    }
    if (l == p || covers(r, l, p, depth + 1)) {
      return true;
    }
  }
}

static bool ieq_n(const char * a, size_t al, const char * b, size_t bl) {
  if (al != bl) {
    return false;
  }
  for (size_t i = 0; i < al; i++) {
    const char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
    const char y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];
    if (x != y) {
      return false;
    }
  }
  return true;
}

/// "the last value in the transition-property in the element's after-change style that matches the property, as described in [the property]": `all` matches every property, a name matches
/// the property it names and, for a shorthand, each of its longhands; `none` matches nothing. -1 for no match.
static long match_index(const GTEXT_CSS_Registry * r, const GTEXT_CSS_Flow * flow, const list_t * props, GTEXT_CSS_Property_Id p, const GTEXT_CSS_Property_Info * pi) {
  for (size_t k = props->n; k-- > 0;) {
    const GTEXT_CSS_Value * v = &props->items[k];
    if (v->kind != GTEXT_CSS_VAL_KEYWORD && v->kind != GTEXT_CSS_VAL_CUSTOM_IDENT) {
      continue;
    }
    if (ieq(v->text.data, v->text.length, "none")) {
      continue;
    }
    if (ieq(v->text.data, v->text.length, "all") || ieq_n(v->text.data, v->text.length, pi->name, pi->name_length)) {
      return (long)k;
    }
    GTEXT_CSS_Property_Id named = gtext_css_registry_find_property(r, v->text.data, v->text.length);
    if (named) {
      /* a flow-relative name stands for the physical property it shares its value with in the element's flow (CSS Logical 1: the two are one property at computed-value time) */
      GTEXT_CSS_Logical_Member lm;
      GTEXT_CSS_Property_Id physical = 0;
      if (gtext_css_logical_member(r, named, &lm) == GTEXT_CSS_OK && lm.logical && gtext_css_logical_map(r, named, flow, &physical) == GTEXT_CSS_OK && physical) {
        named = physical;
        if (named == p) {
          return (long)k;
        }
      }
    }
    if (named && named != p) {
      GTEXT_CSS_Property_Info ni;
      if (gtext_css_registry_property_info(r, named, &ni)) {
        if (ni.alias_of && gtext_css_registry_resolve_alias(r, named) == p) {
          return (long)k;
        }
        if ((ni.flags & GTEXT_CSS_PROP_SHORTHAND) && covers(r, named, p, 0)) {
          return (long)k;
        }
      }
    }
  }
  return -1;
}

/// The computed value of a property of a style, serialized (NUL-terminated, released with the allocator); NULL with GTEXT_CSS_OK for a style that has none.
static GTEXT_CSS_Status value_text(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id id, const GTEXT_Allocator * a, char ** text, size_t * length) {
  *text = NULL;
  *length = 0;
  const GTEXT_CSS_Value * root = gtext_css_computed_root(style, id);
  if (!root) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = a;
  return gtext_css_serialize_value_buffer(root, &so, text, length);
}

/// "the property values are transitionable if they have an animation type that is neither not animatable nor discrete" (and, with transition-behavior: allow-discrete, discrete ones
/// are): the animation type of the property, and then of the pair ("for a particular pair of property values the animation type may be discrete").
static GTEXT_CSS_Status transitionable(const GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id id, const char * a, size_t al, const char * b,
    size_t bl, bool allow_discrete, bool * out) {
  *out = false;
  bool animatable = false;
  const bool discrete_type = css_property_is_discrete(gtext_css_compute_context_registry(cc), id, &animatable);
  if (!animatable) {
    return GTEXT_CSS_OK;
  }
  if (discrete_type) {
    *out = allow_discrete;
    return GTEXT_CSS_OK;
  }
  char * t = NULL;
  size_t n = 0;
  bool discrete = false;
  const GTEXT_CSS_Status st = css_blend_values_text(cc, style, id, a, al, b, bl, 0.5, &cc->alloc, &t, &n, &discrete);
  if (st == GTEXT_CSS_E_INVALID) {
    return GTEXT_CSS_OK;
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &cc->alloc;
  gtext_css_serialize_buffer_free(&so, t);
  *out = !discrete || allow_discrete;
  return GTEXT_CSS_OK;
}

/// One transition: started from `start` (a copy is made) to `end`, with the matching timing item.
static GTEXT_CSS_Status make_tr(GTEXT_CSS_Transition_Set * w, GTEXT_CSS_Property_Id prop, const char * start, size_t ls, const char * end, size_t le, const char * rasv, size_t lr,
    const char * timing, size_t lt, double start_time, double end_time, double rsf, bool allow_discrete) {
  tr_t t;
  memset(&t, 0, sizeof t);
  t.prop = prop;
  t.start = dupn(&w->alloc, start, ls);
  t.ls = ls;
  t.end = dupn(&w->alloc, end, le);
  t.le = le;
  t.rasv = dupn(&w->alloc, rasv, lr);
  t.lr = lr;
  t.timing = dupn(&w->alloc, timing, lt);
  t.lt = lt;
  t.start_time = start_time;
  t.end_time = end_time;
  t.rsf = rsf;
  t.allow_discrete = allow_discrete;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (!t.start || !t.end || !t.rasv || !t.timing) {
    st = GTEXT_CSS_E_OOM;
  }
  else {
    st = run_push(w, &t);
  }
  if (st != GTEXT_CSS_OK) {
    tr_free(&w->alloc, &t);
  }
  return st;
}

static double clamp01(double v) {
  return v < 0 ? 0 : (v > 1 ? 1 : v);
}

/* ---- the update ---------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_transitions_update(const GTEXT_CSS_Compute_Context * context, GTEXT_CSS_Transition_Set * set, const GTEXT_CSS_Computed_Style * before,
    const GTEXT_CSS_Computed_Style * after, double time_ms, GTEXT_CSS_Transition_Update * out) {
  GTEXT_CSS_Transition_Update none;
  memset(&none, 0, sizeof none);
  GTEXT_CSS_Transition_Update * res = out ? out : &none;
  memset(res, 0, sizeof *res);
  if (!context || !set || !after || after->cc != context || (before && before->cc != context)) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Registry * reg = gtext_css_compute_context_registry(context);
  list_t props, durations, delays, timings, behaviors;
  if (!read_list(after, "transition-property", &props) || !read_list(after, "transition-duration", &durations) || !read_list(after, "transition-delay", &delays) ||
      !read_list(after, "transition-timing-function", &timings)) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  const bool has_behavior = read_list(after, "transition-behavior", &behaviors);
  GTEXT_CSS_Transition_Set * w = NULL;
  GTEXT_CSS_Status st = set_copy(set, &w);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const GTEXT_Allocator * al = &w->alloc;
  /* "Running transitions complete at a time that is equal to or after their end time, but prior to the first style change event whose time is equal to or after their end time. When a
     transition completes, implementations must move all transitions that complete at that time from the set of running transitions to the set of completed transitions" */
  for (size_t i = 0; i < w->nrun && st == GTEXT_CSS_OK;) {
    if (w->run[i].end_time <= time_ms) {
      const size_t d = done_find(w, w->run[i].prop);
      if (d != (size_t)-1) {
        done_remove(w, d);
      }
      char * end = w->run[i].end;
      const size_t le = w->run[i].le;
      w->run[i].end = NULL;
      st = done_push(w, w->run[i].prop, end, le);
      if (st != GTEXT_CSS_OK) {
        gtext_allocator_free(al, end);
        break;
      }
      run_remove(w, i);
      res->completed++;
    }
    else {
      i++;
    }
  }
  const GTEXT_CSS_Flow flow = gtext_css_computed_flow(after);
  const size_t nprops = gtext_css_registry_property_count(reg);
  for (GTEXT_CSS_Property_Id id = 1; id <= nprops && st == GTEXT_CSS_OK; id++) {
    GTEXT_CSS_Property_Info pi;
    if (!gtext_css_registry_property_info(reg, id, &pi) || (pi.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS))) {
      continue;
    }
    /* a flow-relative property shares its value with a physical one, which is the property that transitions */
    GTEXT_CSS_Logical_Member lm;
    if (gtext_css_logical_member(reg, id, &lm) == GTEXT_CSS_OK && lm.logical) {
      continue;
    }
    const long m = match_index(reg, &flow, &props, id, &pi);
    size_t ri = run_find(w, id);
    size_t di = done_find(w, id);
    if (m < 0 && ri == (size_t)-1 && di == (size_t)-1) {
      continue;
    }
    char *bt = NULL, *at = NULL, *cur = NULL;
    size_t bl = 0, atl = 0, cl = 0;
    if (before) {
      st = value_text(before, id, al, &bt, &bl);
    }
    if (st == GTEXT_CSS_OK) {
      st = value_text(after, id, al, &at, &atl);
    }
    GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
    so.allocator = al;
    if (st == GTEXT_CSS_OK && !at) {
      /* a property the style has no value of: nothing to transition to */
      gtext_css_serialize_buffer_free(&so, bt);
      continue;
    }
    /* the matching transition duration, delay and timing function are the values of the same place in their lists ("the rules on matching lists": repeated to the length of the list) */
    double dur = 0, delay = 0;
    bool allow_discrete = false;
    char * timing = NULL;
    size_t lt = 0;
    if (st == GTEXT_CSS_OK && m >= 0) {
      const size_t k = (size_t)m;
      if (!time_of(&durations.items[k % durations.n], &dur) || !time_of(&delays.items[k % delays.n], &delay)) {
        st = GTEXT_CSS_E_UNSUPPORTED;
      }
      allow_discrete = has_behavior && is_kw(&behaviors.items[k % behaviors.n], "allow-discrete");
      if (st == GTEXT_CSS_OK) {
        GTEXT_CSS_Serialize_Options so2 = gtext_css_serialize_options_default();
        so2.allocator = al;
        st = gtext_css_serialize_value_buffer(&timings.items[k % timings.n], &so2, &timing, &lt);
      }
    }
    /* "Define the combined duration of the transition as the sum of max(matching transition duration, 0s) and the matching transition delay." */
    const double d0 = dur < 0 ? 0 : dur;
    const double combined = d0 + delay;
    const bool animated = gtext_css_computed_animation_applied(after, id) || (before && gtext_css_computed_animation_applied(before, id));
    bool tn = false;
    if (st == GTEXT_CSS_OK && m >= 0 && bt && !animated && !same(bt, bl, at, atl)) {
      st = transitionable(context, after, id, bt, bl, at, atl, allow_discrete, &tn);
    }
    /* 1. start a transition on a changed property */
    bool started_here = false;
    if (st == GTEXT_CSS_OK && ri == (size_t)-1 && m >= 0 && tn && combined > 0 && !(di != (size_t)-1 && same(w->done[di].end, w->done[di].le, at, atl))) {
      if (di != (size_t)-1) {
        done_remove(w, di);
        di = (size_t)-1;
        res->forgotten++;
      }
      st = make_tr(w, id, bt, bl, at, atl, bt, bl, timing, lt, time_ms + delay, time_ms + delay + d0, 1.0, allow_discrete);
      if (st == GTEXT_CSS_OK) {
        res->started++;
        started_here = true;
      }
    }
    /* 2. a completed transition whose end value is not the after-change value is forgotten */
    if (st == GTEXT_CSS_OK && !started_here && di != (size_t)-1 && !same(w->done[di].end, w->done[di].le, at, atl)) {
      done_remove(w, di);
      di = (size_t)-1;
      res->forgotten++;
    }
    /* 3. "If the element has a running transition or completed transition for the property, and there is not a matching transition-property value, then implementations must cancel the
       running transition or remove the completed transition from the set of completed transitions." */
    if (st == GTEXT_CSS_OK && m < 0) {
      if (ri != (size_t)-1) {
        run_remove(w, ri);
        ri = (size_t)-1;
        res->cancelled++;
      }
      if (di != (size_t)-1) {
        done_remove(w, di);
        res->forgotten++;
      }
    }
    /* 4. a running transition whose end value is not the after-change value */
    if (st == GTEXT_CSS_OK && m >= 0 && ri != (size_t)-1 && !same(w->run[ri].end, w->run[ri].le, at, atl)) {
      tr_t old = w->run[ri];   /* shallow: the strings stay owned by the entry until it is removed */
      st = tr_current(context, after, &old, time_ms, al, &cur, &cl);
      bool cur_ok = false;
      if (st == GTEXT_CSS_OK && !same(cur, cl, at, atl)) {
        st = transitionable(context, after, id, cur, cl, at, atl, allow_discrete, &cur_ok);
      }
      if (st == GTEXT_CSS_OK) {
        if (!cur_ok || combined <= 0) {
          /* "if the current value of the property in the running transition is equal to the value of the property in the after-change style, or if these two values are not transitionable,
             then implementations must cancel the running transition", and the same "if the combined duration is less than or equal to 0s" */
          run_remove(w, ri);
          res->cancelled++;
        }
        else if (same(old.rasv, old.lr, at, atl)) {
          /* the reversing-adjusted start value is the after-change value: cancel and start a transition whose reversing shortening factor is "the absolute value, clamped to the range
             [0, 1], of the sum of: the output of the timing function of the old transition at the time of the style change event, times the reversing shortening factor of the old
             transition; and 1 minus the reversing shortening factor of the old transition." */
          GTEXT_CSS_Easing * e = NULL;
          st = css_easing_from_text(reg, al, old.timing, old.lt, &e);
          if (st == GTEXT_CSS_OK) {
            double progress = 0;
            if (time_ms >= old.end_time) {
              progress = 1;
            }
            else if (time_ms > old.start_time && old.end_time > old.start_time) {
              progress = (time_ms - old.start_time) / (old.end_time - old.start_time);
            }
            const double out_v = gtext_css_easing_output(e, progress, false);
            gtext_css_easing_free(e);
            const double sum = out_v * old.rsf + (1.0 - old.rsf);
            const double rsf = clamp01(sum < 0 ? -sum : sum);
            /* the new transition's start time: the delay when it is not negative, else the product of the shortening factor and the delay */
            const double start_time = time_ms + (delay >= 0 ? delay : rsf * delay);
            char * rasv = dupn(al, old.end, old.le);
            if (!rasv) {
              st = GTEXT_CSS_E_OOM;
            }
            else {
              const size_t lr = old.le;
              st = make_tr(w, id, cur, cl, at, atl, rasv, lr, timing, lt, start_time, start_time + d0 * rsf, rsf, allow_discrete);
              gtext_allocator_free(al, rasv);
              if (st == GTEXT_CSS_OK) {
                /* the new entry is last; the old one is removed (its index has not moved) */
                run_remove(w, ri);
                res->cancelled++;
                res->started++;
              }
            }
          }
        }
        else {
          st = make_tr(w, id, cur, cl, at, atl, cur, cl, timing, lt, time_ms + delay, time_ms + delay + d0, 1.0, allow_discrete);
          if (st == GTEXT_CSS_OK) {
            run_remove(w, ri);
            res->cancelled++;
            res->started++;
          }
        }
      }
    }
    gtext_css_serialize_buffer_free(&so, bt);
    gtext_css_serialize_buffer_free(&so, at);
    gtext_css_serialize_buffer_free(&so, cur);
    gtext_css_serialize_buffer_free(&so, timing);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_transition_set_free(w);
    return st;
  }
  if (res->started || res->cancelled || res->completed || res->forgotten) {
    set_take(set, w);
    set->version++;
  }
  gtext_css_transition_set_free(w);
  return GTEXT_CSS_OK;
}
