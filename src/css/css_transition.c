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
#include "css_creg.h"
#include "css_substitute_internal.h"

/* ---- the set -------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSS_Property_Id prop;     ///< A longhand, or 0 for a custom property ...
  char * cname; size_t lcn;       ///< ... whose name this is (owned; NULL for a longhand).
  char * start; size_t ls;
  char * end; size_t le;
  char * rasv; size_t lr;     ///< the reversing-adjusted start value
  char * timing; size_t lt;   ///< the matching timing function, serialized
  double start_time, end_time, rsf;
  bool allow_discrete;
} tr_t;

typedef struct {
  GTEXT_CSS_Property_Id prop;
  char * cname; size_t lcn;
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
  gtext_allocator_free((GTEXT_Allocator *)a, t->cname);
  gtext_allocator_free((GTEXT_Allocator *)a, t->start);
  gtext_allocator_free((GTEXT_Allocator *)a, t->end);
  gtext_allocator_free((GTEXT_Allocator *)a, t->rasv);
  gtext_allocator_free((GTEXT_Allocator *)a, t->timing);
  memset(t, 0, sizeof *t);
}

static void done_free(const GTEXT_Allocator * a, done_t * d) {
  gtext_allocator_free((GTEXT_Allocator *)a, d->cname);
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
  out->custom_name = (GTEXT_CSS_String){t->cname, t->lcn};
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

bool gtext_css_transition_set_completed_at(const GTEXT_CSS_Transition_Set * set, size_t index, GTEXT_CSS_Property_Id * property, GTEXT_CSS_String * custom_name, GTEXT_CSS_String * end_value) {
  if (!set || index >= set->ndone) {
    return false;
  }
  if (property) {
    *property = set->done[index].prop;
  }
  if (custom_name) {
    *custom_name = (GTEXT_CSS_String){set->done[index].cname, set->done[index].lcn};
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
    b->cname = a->lcn ? dupn(&c->alloc, a->cname, a->lcn) : NULL;
    b->start = dupn(&c->alloc, a->start, a->ls);
    b->end = dupn(&c->alloc, a->end, a->le);
    b->rasv = dupn(&c->alloc, a->rasv, a->lr);
    b->timing = dupn(&c->alloc, a->timing, a->lt);
    c->nrun = i + 1;
    if ((a->lcn && !b->cname) || !b->start || !b->end || !b->rasv || !b->timing) {
      gtext_css_transition_set_free(c);
      return GTEXT_CSS_E_OOM;
    }
  }
  for (size_t i = 0; i < src->ndone; i++) {
    c->done[i].prop = src->done[i].prop;
    c->done[i].le = src->done[i].le;
    c->done[i].lcn = src->done[i].lcn;
    c->done[i].cname = src->done[i].lcn ? dupn(&c->alloc, src->done[i].cname, src->done[i].lcn) : NULL;
    c->done[i].end = dupn(&c->alloc, src->done[i].end, src->done[i].le);
    c->ndone = i + 1;
    if (!c->done[i].end || (src->done[i].lcn && !c->done[i].cname)) {
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

static GTEXT_CSS_Status done_push(GTEXT_CSS_Transition_Set * s, GTEXT_CSS_Property_Id prop, char * cname, size_t lcn, char * end, size_t le) {
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
  s->done[s->ndone].cname = cname;
  s->done[s->ndone].lcn = lcn;
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

static bool same(const char * a, size_t al, const char * b, size_t bl) {
  return al == bl && (al == 0 || memcmp(a, b, al) == 0);
}

/// The property a transition is of: a longhand by its id, a custom property by its name (names of custom properties are case-sensitive).
typedef struct {
  GTEXT_CSS_Property_Id id;   ///< A longhand, or 0.
  const char * name;          ///< The custom property's name when `id` is 0.
  size_t nlen;
} pkey;

static bool key_is(GTEXT_CSS_Property_Id p, const char * cn, size_t cl, pkey k) {
  return p || k.id ? p == k.id : same(cn, cl, k.name, k.nlen);
}

static size_t run_find(const GTEXT_CSS_Transition_Set * s, pkey k) {
  for (size_t i = 0; i < s->nrun; i++) {
    if (key_is(s->run[i].prop, s->run[i].cname, s->run[i].lcn, k)) {
      return i;
    }
  }
  return (size_t)-1;
}

static size_t done_find(const GTEXT_CSS_Transition_Set * s, pkey k) {
  for (size_t i = 0; i < s->ndone; i++) {
    if (key_is(s->done[i].prop, s->done[i].cname, s->done[i].lcn, k)) {
      return i;
    }
  }
  return (size_t)-1;
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
  return css_blend_key_text(cc, style, t->prop, t->cname, t->lcn, t->start, t->ls, t->end, t->le, p, a, text, length, &discrete);
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

/// "the last value in the transition-property in the element's after-change style that matches the property" for a custom property: an item that names it (names of custom properties are case-sensitive:
/// a <custom-ident>), or `all` when the property has a type ("all" is every property that can be animated, and a custom property that is not typed is discrete, which transitions only by being named).
static long match_custom(const list_t * props, pkey k, bool typed) {
  for (size_t i = props->n; i-- > 0;) {
    const GTEXT_CSS_Value * v = &props->items[i];
    if (v->kind != GTEXT_CSS_VAL_KEYWORD && v->kind != GTEXT_CSS_VAL_CUSTOM_IDENT) {
      continue;
    }
    if (ieq(v->text.data, v->text.length, "none")) {
      continue;
    }
    if (ieq(v->text.data, v->text.length, "all")) {
      if (typed) {
        return (long)i;
      }
      continue;
    }
    if (same(v->text.data, v->text.length, k.name, k.nlen)) {
      return (long)i;
    }
  }
  return -1;
}

typedef struct {
  const char * p;
  size_t n;
} cname_t;

static int cname_cmp(const void * a, const void * b) {
  const cname_t * x = a;
  const cname_t * y = b;
  const size_t n = x->n < y->n ? x->n : y->n;
  const int c = n ? memcmp(x->p, y->p, n) : 0;
  return c ? c : (x->n > y->n) - (x->n < y->n);
}

/// The custom properties an update looks at: the names a `transition-property` item gives, the typed registered ones when it has `all`, and those with a running or completed transition. Copies of
/// the names, sorted and without repeats; `*count` of them (the arrays are released with the set's allocator).
static GTEXT_CSS_Status custom_candidates(const GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Transition_Set * w, const list_t * props, char *** names, size_t ** lens, size_t * count) {
  *names = NULL;
  *lens = NULL;
  *count = 0;
  bool all = false;
  for (size_t i = 0; i < props->n; i++) {
    const GTEXT_CSS_Value * v = &props->items[i];
    all = all || ((v->kind == GTEXT_CSS_VAL_KEYWORD || v->kind == GTEXT_CSS_VAL_CUSTOM_IDENT) && ieq(v->text.data, v->text.length, "all"));
  }
  const size_t cap = props->n + w->nrun + w->ndone + (all ? cc->nregs : 0);
  if (!cap) {
    return GTEXT_CSS_OK;
  }
  GTEXT_Allocator a = w->alloc;
  cname_t * tmp = gtext_allocator_malloc(&a, cap * sizeof *tmp);
  if (!tmp) {
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  for (size_t i = 0; i < props->n; i++) {
    const GTEXT_CSS_Value * v = &props->items[i];
    if ((v->kind == GTEXT_CSS_VAL_KEYWORD || v->kind == GTEXT_CSS_VAL_CUSTOM_IDENT) && v->text.length >= 3 && v->text.data[0] == '-' && v->text.data[1] == '-') {
      tmp[n++] = (cname_t){v->text.data, v->text.length};
    }
  }
  for (size_t i = 0; all && i < cc->nregs; i++) {
    const css_creg * cr = cc->regs[i].creg;
    if (!cr->syntax->universal) {
      tmp[n++] = (cname_t){cr->name, cr->name_len};
    }
  }
  for (size_t i = 0; i < w->nrun; i++) {
    if (w->run[i].lcn) {
      tmp[n++] = (cname_t){w->run[i].cname, w->run[i].lcn};
    }
  }
  for (size_t i = 0; i < w->ndone; i++) {
    if (w->done[i].lcn) {
      tmp[n++] = (cname_t){w->done[i].cname, w->done[i].lcn};
    }
  }
  if (n > 1) {
    qsort(tmp, n, sizeof *tmp, cname_cmp);
    size_t k = 1;
    for (size_t i = 1; i < n; i++) {
      if (cname_cmp(&tmp[i], &tmp[k - 1]) != 0) {
        tmp[k++] = tmp[i];
      }
    }
    n = k;
  }
  char ** nm = n ? gtext_allocator_calloc(&a, n, sizeof *nm) : NULL;
  size_t * ln = n ? gtext_allocator_calloc(&a, n, sizeof *ln) : NULL;
  GTEXT_CSS_Status st = n && (!nm || !ln) ? GTEXT_CSS_E_OOM : GTEXT_CSS_OK;
  for (size_t i = 0; st == GTEXT_CSS_OK && i < n; i++) {
    nm[i] = dupn(&a, tmp[i].p, tmp[i].n);
    ln[i] = tmp[i].n;
    if (!nm[i]) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  gtext_allocator_free(&a, tmp);
  if (st != GTEXT_CSS_OK) {
    for (size_t i = 0; i < n && nm; i++) {
      gtext_allocator_free(&a, nm[i]);
    }
    gtext_allocator_free(&a, nm);
    gtext_allocator_free(&a, ln);
    return st;
  }
  *names = nm;
  *lens = ln;
  *count = n;
  return GTEXT_CSS_OK;
}

/// The computed value of a property of a style, serialized (NUL-terminated, released with the allocator); NULL with GTEXT_CSS_OK for a style that has none. A custom property the style has no
/// value of has the guaranteed-invalid value, which has no text of its own (an empty text is the value of `--x:;`): it is written `initial`, the declaration that gives it, which no value can be
/// ("initial" is a CSS-wide keyword and never a computed value), so that a current value that is the guaranteed-invalid value is declared as itself.
static GTEXT_CSS_Status value_text(const GTEXT_CSS_Computed_Style * style, pkey k, const GTEXT_Allocator * a, char ** text, size_t * length) {
  *text = NULL;
  *length = 0;
  if (!k.id) {
    const GTEXT_CSS_Custom_Value * cv = NULL;
    if (!gtext_css_computed_custom_find(style, k.name, k.nlen, &cv) || !cv) {
      *text = dupn(a, "initial", 7);
      *length = 7;
      return *text ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    }
    GTEXT_CSS_Serialize_Options co = gtext_css_serialize_options_default();
    co.allocator = a;
    return gtext_css_custom_value_text(cv, &co, text, length);
  }
  const GTEXT_CSS_Value * root = gtext_css_computed_root(style, k.id);
  if (!root) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = a;
  return gtext_css_serialize_value_buffer(root, &so, text, length);
}

/// Whether a custom property is one a transition interpolates: registered with a syntax that is not the universal one ("the animation type is by computed value, derived from the type used in the
/// property's syntax definition. Where there is no computed value type that corresponds to the property's specified syntax (e.g. when the syntax is the universal syntax definition) or when the custom
/// property is not registered, the animation type is discrete", Web Animations 1).
static bool custom_typed(const GTEXT_CSS_Compute_Context * cc, const char * name, size_t nlen) {
  const css_ereg * reg = cc->nregs ? css_ctx_find_reg(cc, name, nlen, NULL) : NULL;
  return reg && !reg->creg->syntax->universal;
}

/// "the property values are transitionable if they have an animation type that is neither not animatable nor discrete" (and, with transition-behavior: allow-discrete, discrete ones
/// are): the animation type of the property, and then of the pair ("for a particular pair of property values the animation type may be discrete").
static GTEXT_CSS_Status transitionable(const GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Computed_Style * style, pkey k, const char * a, size_t al, const char * b,
    size_t bl, bool allow_discrete, bool * out) {
  *out = false;
  if (k.id) {
    bool animatable = false;
    const bool discrete_type = css_property_is_discrete(gtext_css_compute_context_registry(cc), k.id, &animatable);
    if (!animatable) {
      return GTEXT_CSS_OK;
    }
    if (discrete_type) {
      *out = allow_discrete;
      return GTEXT_CSS_OK;
    }
  }
  else if (!custom_typed(cc, k.name, k.nlen)) {
    *out = allow_discrete;   /* an unregistered or universal custom property is discrete */
    return GTEXT_CSS_OK;
  }
  char * t = NULL;
  size_t n = 0;
  bool discrete = false;
  const GTEXT_CSS_Status st = css_blend_key_text(cc, style, k.id, k.name, k.nlen, a, al, b, bl, 0.5, &cc->alloc, &t, &n, &discrete);
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
static GTEXT_CSS_Status make_tr(GTEXT_CSS_Transition_Set * w, pkey key, const char * start, size_t ls, const char * end, size_t le, const char * rasv, size_t lr,
    const char * timing, size_t lt, double start_time, double end_time, double rsf, bool allow_discrete) {
  tr_t t;
  memset(&t, 0, sizeof t);
  t.prop = key.id;
  t.cname = key.id ? NULL : dupn(&w->alloc, key.name, key.nlen);
  t.lcn = key.id ? 0 : key.nlen;
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
  if ((!key.id && !t.cname) || !t.start || !t.end || !t.rasv || !t.timing) {
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

/// Whether a CSS Animation of the element applies a property in a style (a longhand by its id, a custom property by its name).
static bool animation_applied(const GTEXT_CSS_Computed_Style * style, pkey k) {
  if (k.id) {
    return gtext_css_computed_animation_applied(style, k.id);
  }
  for (size_t i = 0; i < style->anim_applied.nnames; i++) {
    if (same(style->anim_applied.names[i], style->anim_applied.name_lengths[i], k.name, k.nlen)) {
      return true;
    }
  }
  return false;
}

/// What one update works over: the two styles, the lists of the after-change style (matched by place), the set being built.
typedef struct {
  const GTEXT_CSS_Compute_Context * context;
  const GTEXT_CSS_Registry * reg;
  const GTEXT_CSS_Computed_Style * before;
  const GTEXT_CSS_Computed_Style * after;
  double time_ms;
  GTEXT_CSS_Transition_Set * w;
  list_t durations, delays, timings, behaviors;
  bool has_behavior;
  GTEXT_CSS_Transition_Update * res;
} upd_t;

/// "Starting of transitions" for one property of one style change event: `m` is the index of the matching `transition-property` item (-1 for none). The steps are the ones of CSS Transitions 1, each
/// quoted where it is followed.
static GTEXT_CSS_Status step_key(upd_t * u, pkey k, long m) {
  const GTEXT_CSS_Compute_Context * context = u->context;
  const GTEXT_CSS_Registry * reg = u->reg;
  const GTEXT_CSS_Computed_Style * before = u->before;
  const GTEXT_CSS_Computed_Style * after = u->after;
  const double time_ms = u->time_ms;
  GTEXT_CSS_Transition_Set * w = u->w;
  const GTEXT_Allocator * al = &w->alloc;
  const list_t durations = u->durations, delays = u->delays, timings = u->timings, behaviors = u->behaviors;
  const bool has_behavior = u->has_behavior;
  GTEXT_CSS_Transition_Update * res = u->res;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  size_t ri = run_find(w, k);
  size_t di = done_find(w, k);
  if (m < 0 && ri == (size_t)-1 && di == (size_t)-1) {
    return GTEXT_CSS_OK;
  }
  char *bt = NULL, *at = NULL, *cur = NULL;
  size_t bl = 0, atl = 0, cl = 0;
  if (before) {
    st = value_text(before, k, al, &bt, &bl);
  }
  if (st == GTEXT_CSS_OK) {
    st = value_text(after, k, al, &at, &atl);
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = al;
  if (st == GTEXT_CSS_OK && !at) {
    /* a property the style has no value of: nothing to transition to */
    gtext_css_serialize_buffer_free(&so, bt);
    return GTEXT_CSS_OK;
  }
  /* the matching transition duration, delay and timing function are the values of the same place in their lists ("the rules on matching lists": repeated to the length of the list) */
  double dur = 0, delay = 0;
  bool allow_discrete = false;
  char * timing = NULL;
  size_t lt = 0;
  if (st == GTEXT_CSS_OK && m >= 0) {
    const size_t mi = (size_t)m;
    if (!time_of(&durations.items[mi % durations.n], &dur) || !time_of(&delays.items[mi % delays.n], &delay)) {
      st = GTEXT_CSS_E_UNSUPPORTED;
    }
    allow_discrete = has_behavior && is_kw(&behaviors.items[mi % behaviors.n], "allow-discrete");
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Serialize_Options so2 = gtext_css_serialize_options_default();
      so2.allocator = al;
      st = gtext_css_serialize_value_buffer(&timings.items[mi % timings.n], &so2, &timing, &lt);
    }
  }
  /* "Define the combined duration of the transition as the sum of max(matching transition duration, 0s) and the matching transition delay." */
  const double d0 = dur < 0 ? 0 : dur;
  const double combined = d0 + delay;
  const bool animated = animation_applied(after, k) || (before && animation_applied(before, k));
  bool tn = false;
  if (st == GTEXT_CSS_OK && m >= 0 && bt && !animated && !same(bt, bl, at, atl)) {
    st = transitionable(context, after, k, bt, bl, at, atl, allow_discrete, &tn);
  }
  /* 1. start a transition on a changed property */
  bool started_here = false;
  if (st == GTEXT_CSS_OK && ri == (size_t)-1 && m >= 0 && tn && combined > 0 && !(di != (size_t)-1 && same(w->done[di].end, w->done[di].le, at, atl))) {
    if (di != (size_t)-1) {
      done_remove(w, di);
      di = (size_t)-1;
      res->forgotten++;
    }
    st = make_tr(w, k, bt, bl, at, atl, bt, bl, timing, lt, time_ms + delay, time_ms + delay + d0, 1.0, allow_discrete);
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
      st = transitionable(context, after, k, cur, cl, at, atl, allow_discrete, &cur_ok);
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
            st = make_tr(w, k, cur, cl, at, atl, rasv, lr, timing, lt, start_time, start_time + d0 * rsf, rsf, allow_discrete);
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
        st = make_tr(w, k, cur, cl, at, atl, cur, cl, timing, lt, time_ms + delay, time_ms + delay + d0, 1.0, allow_discrete);
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
  return st;
}

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
      const pkey rk = {w->run[i].prop, w->run[i].cname, w->run[i].lcn};
      const size_t d = done_find(w, rk);
      if (d != (size_t)-1) {
        done_remove(w, d);
      }
      char * end = w->run[i].end;
      const size_t le = w->run[i].le;
      char * cname = w->run[i].cname;
      const size_t lcn = w->run[i].lcn;
      w->run[i].end = NULL;
      w->run[i].cname = NULL;
      st = done_push(w, w->run[i].prop, cname, lcn, end, le);
      if (st != GTEXT_CSS_OK) {
        gtext_allocator_free(al, end);
        gtext_allocator_free(al, cname);
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
  upd_t u = {context, reg, before, after, time_ms, w, durations, delays, timings, behaviors, has_behavior, res};
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
    pkey k = {id, NULL, 0};
    st = step_key(&u, k, m);
  }
  /* the custom properties a transition can be of: those a `transition-property` item names (`all` names the registered ones that have a type), and those that have a transition running or completed
     (so that one no longer named is cancelled). Nothing is asked of a custom property of the style that none of these says: a document with a thousand of them pays for the ones it transitions. */
  char ** names = NULL;
  size_t *lens = NULL, nnames = 0;
  st = st == GTEXT_CSS_OK ? custom_candidates(context, w, &props, &names, &lens, &nnames) : st;
  for (size_t n = 0; n < nnames && st == GTEXT_CSS_OK; n++) {
    pkey k = {0, names[n], lens[n]};
    const bool typed = custom_typed(context, k.name, k.nlen);
    st = step_key(&u, k, match_custom(&props, k, typed));
  }
  for (size_t n = 0; n < nnames; n++) {
    gtext_allocator_free((GTEXT_Allocator *)al, names[n]);
  }
  gtext_allocator_free((GTEXT_Allocator *)al, names);
  gtext_allocator_free((GTEXT_Allocator *)al, lens);
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
