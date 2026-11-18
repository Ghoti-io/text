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

/// The most cancelled or completed transitions a set holds for the next frame to report.
#define MAX_OWED 4096

typedef struct {
  GTEXT_CSS_Property_Id prop;     ///< A longhand, or 0 for a custom property ...
  char * cname; size_t lcn;       ///< ... whose name this is (owned; NULL for a longhand).
  char * start; size_t ls;
  char * end; size_t le;
  char * rasv; size_t lr;     ///< the reversing-adjusted start value
  char * timing; size_t lt;   ///< the matching timing function, serialized
  double start_time, end_time, rsf;
  bool allow_discrete;
  char * pname; size_t lpn;   ///< the expanded transition property name (owned)
  uint64_t id, generation;
  double delay;               ///< the start delay of the effect: start time minus the time of the style change event
  GTEXT_CSS_Transition_Phase phase;   ///< the phase the last frame left it in (idle until a frame sees it)
  bool ready;                 ///< a frame has seen it ("the animation is ready")
  bool start_tainted, end_tainted;   ///< a custom property: the start (end) value came from an attr()-tainted value (Values 5, attr() security), which its current value keeps
  GTEXT_Anim_Animation * m;   ///< borrowed: the Animation that holds its timing state once script has touched it (gtext_css_transition_set_bind()); NULL before. The owner of the set owns it.
  bool css_task;              ///< the pending play task of the Animation is one the style change event made, which is ready at the time of that event (a task script makes is ready at the frame)
  uint32_t css_task_gen;      ///< the ready generation of the Animation when that task was made: a task of a later generation is script's
  bool pending_phase;         ///< the phase the last frame left it in is "pending" (the phase before the effect, with a play task waiting), which a frame that finds the task run moves on from
  double active_both;         ///< the active time with a fill of both at the last frame that saw a current time (what a transitioncancel reports when script cancelled the Animation)
} tr_t;

/// A transition that left the running set and has an event the next frame owes: a transitioncancel (cancelled after a frame saw it begin and before it ended), or the events up to transitionend
/// (completed before a frame saw it end).
typedef struct {
  char * pname; size_t lpn;
  GTEXT_CSS_Property_Id prop;
  uint64_t id, generation;
  double start_time, end_time, delay;
  double elapsed;             ///< a cancellation: the active time at the moment, in ms
  double left_at;             ///< the time it left the running set (a cancellation's scheduled event time)
  GTEXT_CSS_Transition_Phase phase;   ///< the phase the last frame saw it in
  bool cancelled;
  bool held;                  ///< an Animation held it: the scheduled times of its events are the Animation's (tr_sched_basis(), as it was when the transition left)
  double sbase, sinv;
  bool svalid;
} gone_t;

typedef struct {
  GTEXT_CSS_Property_Id prop;
  char * cname; size_t lcn;
  char * end; size_t le;
} done_t;

/// A list of Animations the owner of a set will call when an update has succeeded.
typedef struct {
  GTEXT_Anim_Animation ** items;
  size_t n, cap;
} alist;

struct GTEXT_CSS_Transition_Set {
  GTEXT_Allocator alloc;
  tr_t * run;
  size_t nrun, caprun;
  done_t * done;
  size_t ndone, capdone;
  gone_t * gone;
  size_t ngone, capgone;
  uint64_t version;
  uint64_t next_id;       ///< the last transition id given
  uint64_t lost;          ///< owed events dropped past MAX_OWED
  uint64_t generation;    ///< the "current transition generation" of this element: one for each style change event that changed the set
  alist pc;               ///< the Animations a running update (or gtext_css_transition_set_cancel_all()) cancels when it is done: a cancel that is made late cannot be undone, so it waits for the update to succeed
  alist pf;               ///< the Animations of the transitions that completed: "update an animation's finished state" is run for each when the update is done
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
  gtext_allocator_free((GTEXT_Allocator *)a, t->pname);
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

static void gone_free(const GTEXT_Allocator * a, gone_t * g) {
  gtext_allocator_free((GTEXT_Allocator *)a, g->pname);
  memset(g, 0, sizeof *g);
}

static void contents_free(GTEXT_CSS_Transition_Set * s) {
  for (size_t i = 0; i < s->ngone; i++) {
    gone_free(&s->alloc, &s->gone[i]);
  }
  gtext_allocator_free(&s->alloc, s->gone);
  s->gone = NULL;
  s->ngone = s->capgone = 0;
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
  gtext_allocator_free(&a, set->pc.items);
  gtext_allocator_free(&a, set->pf.items);
  gtext_allocator_free(&a, set);
}

void gtext_css_transition_set_clear(GTEXT_CSS_Transition_Set * set) {
  if (set && (set->nrun || set->ndone || set->ngone)) {
    contents_free(set);
    set->version++;
  }
}

uint64_t gtext_css_transition_set_version(const GTEXT_CSS_Transition_Set * set) {
  return set ? set->version : 0;
}

uint64_t gtext_css_transition_set_last_id(const GTEXT_CSS_Transition_Set * set) {
  return set ? set->next_id : 0;
}

void gtext_css_transition_set_start_ids_after(GTEXT_CSS_Transition_Set * set, uint64_t id) {
  if (set && set->next_id < id) {
    set->next_id = id;
  }
}

uint64_t gtext_css_transition_set_events_lost(const GTEXT_CSS_Transition_Set * set) {
  return set ? set->lost : 0;
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
  out->id = t->id;
  out->generation = t->generation;
  out->property_name = (GTEXT_CSS_String){t->pname, t->lpn};
  out->delay_ms = t->delay;
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
  c->next_id = src->next_id;
  c->generation = src->generation;
  c->lost = src->lost;
  if (src->ngone) {
    c->gone = gtext_allocator_calloc(&c->alloc, src->ngone, sizeof *c->gone);
    if (!c->gone) {
      gtext_css_transition_set_free(c);
      return GTEXT_CSS_E_OOM;
    }
    c->capgone = src->ngone;
    for (size_t i = 0; i < src->ngone; i++) {
      c->gone[i] = src->gone[i];
      c->gone[i].pname = dupn(&c->alloc, src->gone[i].pname, src->gone[i].lpn);
      c->ngone = i + 1;
      if (!c->gone[i].pname) {
        gtext_css_transition_set_free(c);
        return GTEXT_CSS_E_OOM;
      }
    }
  }
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
    b->pname = dupn(&c->alloc, a->pname, a->lpn);
    c->nrun = i + 1;
    if ((a->lcn && !b->cname) || !b->start || !b->end || !b->rasv || !b->timing || !b->pname) {
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
  dst->gone = src->gone;
  dst->ngone = src->ngone;
  dst->capgone = src->capgone;
  dst->next_id = src->next_id;
  dst->generation = src->generation;
  dst->lost = src->lost;
  src->run = NULL;
  src->done = NULL;
  src->gone = NULL;
  src->nrun = src->caprun = src->ndone = src->capdone = src->ngone = src->capgone = 0;
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

/// The phase of the effect of a transition at a time (Web Animations, "Animation effect phases and states"; the effect has no end delay, a fill of backwards, one iteration and a forwards direction):
/// before while the time is before the start time, after from the end time (an effect with no active duration is never active), active between.
static GTEXT_CSS_Transition_Phase phase_at(double start_time, double end_time, double time_ms) {
  if (time_ms < start_time) {
    return GTEXT_CSS_TRANSITION_PHASE_BEFORE;
  }
  if (time_ms >= end_time) {
    return GTEXT_CSS_TRANSITION_PHASE_AFTER;
  }
  return GTEXT_CSS_TRANSITION_PHASE_ACTIVE;
}

static double clamp_range(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- the Animation of a transition --------------------------------------------------------------------------------------------------------------- */

/// The time of the style change event that made the transition: when its animation started, in the model of a record that holds its own timing (the start time of the animation, which the delay follows).
static double tr_event_time(const tr_t * t) {
  return t->start_time - t->delay;
}

/// The state of the Animation that holds a transition (the transition must have one) at a time.
static GTEXT_Anim_Animation_State m_state(const tr_t * t, double now) {
  GTEXT_Anim_Animation_State s;
  (void)gtext_anim_animation_state(t->m, (GTEXT_Anim_Time){true, now}, &s);
  return s;
}

/// The timing of the effect of a transition (Transitions 2, "Starting of transitions": the transition duration is the iteration duration, the transition delay the start delay, the timing function the
/// effect's easing; "the transition effect has a fill of backwards" and one iteration, no end delay, a normal direction). The easing is not given: the model reads the times, and the record reads
/// its own timing function for the value.
static GTEXT_Anim_Effect_Timing tr_timing(const tr_t * t) {
  GTEXT_Anim_Effect_Timing tm = gtext_anim_timing_default();
  tm.delay = t->delay;
  tm.fill = GTEXT_ANIM_FILL_BACKWARDS;
  tm.duration_auto = false;
  tm.duration = t->end_time - t->start_time;
  return tm;
}

/// The time the arithmetic of the record reads at `now`, on the clock of its start and end times: `now` itself for a record that holds its own timing, and the time of the style change event plus the
/// current time of the Animation for one an Animation holds (so that a seek, a pause, a reversal and a playback rate are all the Animation's, and the record's times are only where its effect is on
/// the Animation's clock). False when the Animation is idle (it has no current time, so the effect has no result: `time` is then the end time, whose value is the one the style has without the transition).
static bool tr_time(const tr_t * t, double now, double * time) {
  if (!t->m) {
    *time = now;
    return true;
  }
  const GTEXT_Anim_Animation_State s = m_state(t, now);
  if (!s.current_time.resolved) {
    *time = t->end_time;
    return false;
  }
  *time = tr_event_time(t) + s.current_time.ms;
  return true;
}

/// "The active time of the animation ... calculated using a fill mode of both", in ms, at a local time.
static double tr_active_both(const tr_t * t, GTEXT_Anim_Time local, double rate) {
  GTEXT_Anim_Effect_Timing tm = tr_timing(t);
  tm.fill = GTEXT_ANIM_FILL_BOTH;
  GTEXT_Anim_Computed_Timing ct;
  if (gtext_anim_computed_timing(&tm, local, rate, false, &ct) != GTEXT_ANIM_OK || !ct.in_effect) {
    return 0.0;
  }
  return ct.active_time;
}

/// The active time (fill both) a cancellation at `now` reports: for a record that holds its own timing the time since the style change event clamped to the effect, for one an Animation holds
/// the active time at its current time, or at the last frame's when script already made it idle.
static double tr_elapsed_at(const tr_t * t, double now) {
  if (!t->m) {
    return clamp_range(now - t->start_time, 0, t->end_time - t->start_time);
  }
  const GTEXT_Anim_Animation_State s = m_state(t, now);
  return s.current_time.resolved ? tr_active_both(t, s.current_time, s.playback_rate) : t->active_both;
}

/// Room for `more` entries that left the running set (so that moving one cannot fail).
static GTEXT_CSS_Status gone_reserve(GTEXT_CSS_Transition_Set * s, size_t more) {
  /* a set that nobody takes frames of would hold what it owes for ever: the oldest are dropped (and counted) past a bound */
  while (s->ngone + more > MAX_OWED && s->ngone > 0) {
    gone_free(&s->alloc, &s->gone[0]);
    memmove(&s->gone[0], &s->gone[1], (s->ngone - 1) * sizeof *s->gone);
    s->ngone--;
    s->lost++;
  }
  if (s->ngone + more <= s->capgone) {
    return GTEXT_CSS_OK;
  }
  size_t cap = s->capgone ? s->capgone : 4;
  while (cap < s->ngone + more) {
    cap *= 2;
  }
  gone_t * bigger = gtext_allocator_realloc(&s->alloc, s->gone, cap * sizeof *bigger);
  if (!bigger) {
    return GTEXT_CSS_E_OOM;
  }
  s->gone = bigger;
  s->capgone = cap;
  return GTEXT_CSS_OK;
}

/// Room for `more` Animations that the update in progress calls when it is done.
static GTEXT_CSS_Status alist_reserve(const GTEXT_Allocator * alloc, alist * l, size_t more) {
  if (l->n + more <= l->cap) {
    return GTEXT_CSS_OK;
  }
  size_t cap = l->cap ? l->cap : 4;
  while (cap < l->n + more) {
    cap *= 2;
  }
  GTEXT_Anim_Animation ** bigger = gtext_allocator_realloc((GTEXT_Allocator *)alloc, l->items, cap * sizeof *bigger);
  if (!bigger) {
    return GTEXT_CSS_E_OOM;
  }
  l->items = bigger;
  l->cap = cap;
  return GTEXT_CSS_OK;
}

/// How the ideal times of the events of a held transition are put on the timeline: "animation time to timeline time" is `base + time x inv`, with the start time of the Animation and 1 over its playback
/// rate. A task the style change event made and the frame has not run starts the transition at the time of that event (its start time is not resolved yet, as for a record that holds its own state). With
/// no start time, or a rate of 0, there is no ideal time: `valid` is false and the event has the time of the frame.
static void tr_sched_basis(const tr_t * t, const GTEXT_Anim_Animation_State * ms, double * base, double * inv, bool * valid) {
  if (ms->pending && t->css_task && ms->ready_generation == t->css_task_gen) {
    *base = tr_event_time(t);
    *inv = 1.0;
    *valid = true;
    return;
  }
  *base = ms->start_time.ms;
  *inv = ms->playback_rate != 0.0 ? 1.0 / ms->playback_rate : 0.0;
  *valid = ms->start_time.resolved && ms->playback_rate != 0.0;
}

/// Running transition `i` leaves the set at a time, cancelled or completed, and is removed. If a frame has seen it in a phase that owes an event (Transitions 2, "Event dispatch": a cancelled
/// transition that is not idle and not after owes a transitioncancel, with "the active time of the animation at the moment it was canceled calculated using a fill mode of both"; a completed one
/// that is not yet after owes the events up to transitionend) the entry moves to the list of what the next frame reports. Needs room in `gone` for one more (gone_reserve()) when it can fail.
static GTEXT_CSS_Status run_leave(GTEXT_CSS_Transition_Set * s, size_t i, double time_ms, bool cancelled) {
  tr_t * t = &s->run[i];
  /* "run the procedure to cancel an animation on the transition": the Animation that holds it is cancelled by the caller when the whole of what it does has succeeded, and one whose transition completed
     is asked whether it has finished (the room for the two is made here) */
  alist * const after = cancelled ? &s->pc : &s->pf;
  if (t->m) {
    const GTEXT_CSS_Status ps = alist_reserve(&s->alloc, after, 1);
    if (ps != GTEXT_CSS_OK) {
      return ps;
    }
  }
  const bool owes = cancelled ? (t->phase != GTEXT_CSS_TRANSITION_PHASE_IDLE && t->phase != GTEXT_CSS_TRANSITION_PHASE_AFTER) : t->phase != GTEXT_CSS_TRANSITION_PHASE_AFTER;
  if (owes) {
    GTEXT_CSS_Status st = gone_reserve(s, 1);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    gone_t * g = &s->gone[s->ngone++];
    g->pname = t->pname;
    g->lpn = t->lpn;
    t->pname = NULL;
    g->prop = t->prop;
    g->id = t->id;
    g->generation = t->generation;
    g->start_time = t->start_time;
    g->end_time = t->end_time;
    g->delay = t->delay;
    g->phase = t->phase;
    g->cancelled = cancelled;
    g->held = t->m != NULL;
    if (g->held) {
      const GTEXT_Anim_Animation_State ms = m_state(t, time_ms);
      tr_sched_basis(t, &ms, &g->sbase, &g->sinv, &g->svalid);
    }
    g->left_at = time_ms;
    g->elapsed = tr_elapsed_at(t, time_ms);
  }
  if (t->m) {
    after->items[after->n++] = t->m;
  }
  run_remove(s, i);
  return GTEXT_CSS_OK;
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

/// Whether the current value of a transition at a time is attr()-tainted: it is the start value at or before the start time and the end value from the end time, and one made from both (an
/// interpolation, or a discrete flip between them) is tainted when either was: "the substitution value of an arbitrary substitution function is attr()-tainted as a whole if any
/// attr()-tainted values were involved in creating it" (Values 5, attr() security). The branches are those of tr_current().
static bool tr_tainted_at(const tr_t * t, double time_ms) {
  if (time_ms <= t->start_time || t->end_time <= t->start_time) {
    return (time_ms < t->end_time || t->end_time > t->start_time) ? t->start_tainted : t->end_tainted;
  }
  if (time_ms >= t->end_time) {
    return t->end_tainted;
  }
  return t->start_tainted || t->end_tainted;
}

bool gtext_css_transition_current_tainted(const GTEXT_CSS_Transition_Set * set, size_t index, double time_ms) {
  if (!set || index >= set->nrun) {
    return false;
  }
  const tr_t * t = &set->run[index];
  double e;
  return tr_time(t, time_ms, &e) ? tr_tainted_at(t, e) : t->end_tainted;
}

/// The current value of a transition at a time on the clock of the caller: the one at the time its Animation says (see tr_time()), and the end value, which is what the style has, when the
/// Animation is idle.
static GTEXT_CSS_Status tr_current_at(const GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Computed_Style * style, const tr_t * t, double now, const GTEXT_Allocator * alloc, char ** text, size_t * length) {
  double e;
  if (!tr_time(t, now, &e)) {
    const GTEXT_Allocator * a = alloc ? alloc : &cc->alloc;
    *text = dupn(a, t->end, t->le);
    *length = t->le;
    return *text ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  return tr_current(cc, style, t, e, alloc, text, length);
}

GTEXT_CSS_Status gtext_css_transition_current_value(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Computed_Style * style, const GTEXT_CSS_Transition_Set * set, size_t index,
    double time_ms, const GTEXT_Allocator * allocator, char ** text, size_t * length) {
  if (!context || !set || !text || !length || index >= set->nrun) {
    return GTEXT_CSS_E_INVALID;
  }
  return tr_current_at(context, style, &set->run[index], time_ms, allocator, text, length);
}

bool gtext_css_transition_set_in_effect_at(const GTEXT_CSS_Transition_Set * set, size_t index, double time_ms) {
  if (!set || index >= set->nrun) {
    return false;
  }
  double e;
  return tr_time(&set->run[index], time_ms, &e) && e < set->run[index].end_time;
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
static GTEXT_CSS_Status value_text(const GTEXT_CSS_Computed_Style * style, pkey k, const GTEXT_Allocator * a, char ** text, size_t * length, bool * tainted) {
  *text = NULL;
  *length = 0;
  *tainted = false;
  if (!k.id) {
    const GTEXT_CSS_Custom_Value * cv = NULL;
    if (!gtext_css_computed_custom_find(style, k.name, k.nlen, &cv) || !cv) {
      *text = dupn(a, "initial", 7);
      *length = 7;
      return *text ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    }
    GTEXT_CSS_Serialize_Options co = gtext_css_serialize_options_default();
    co.allocator = a;
    *tainted = gtext_css_custom_value_tainted(cv);
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
static GTEXT_CSS_Status make_tr(GTEXT_CSS_Transition_Set * w, const GTEXT_CSS_Registry * reg, pkey key, const char * start, size_t ls, const char * end, size_t le, const char * rasv,
    size_t lr, const char * timing, size_t lt, double event_time, double start_time, double end_time, double rsf, bool allow_discrete, bool start_tainted, bool end_tainted) {
  tr_t t;
  memset(&t, 0, sizeof t);
  t.start_tainted = start_tainted;
  t.end_tainted = end_tainted;
  const char * pname = key.name;
  size_t lpn = key.nlen;
  if (key.id) {
    GTEXT_CSS_Property_Info pi;
    if (!gtext_css_registry_property_info(reg, key.id, &pi)) {
      return GTEXT_CSS_E_INVALID;
    }
    pname = pi.name;
    lpn = pi.name_length;
  }
  t.pname = dupn(&w->alloc, pname, lpn);
  t.lpn = lpn;
  t.id = ++w->next_id;
  t.generation = w->generation;
  t.delay = start_time - event_time;
  t.phase = GTEXT_CSS_TRANSITION_PHASE_IDLE;
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
  if ((!key.id && !t.cname) || !t.start || !t.end || !t.rasv || !t.timing || !t.pname) {
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
  bool bt_tainted = false, at_tainted = false;
  if (before) {
    st = value_text(before, k, al, &bt, &bl, &bt_tainted);
  }
  if (st == GTEXT_CSS_OK) {
    st = value_text(after, k, al, &at, &atl, &at_tainted);
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
    st = make_tr(w, reg, k, bt, bl, at, atl, bt, bl, timing, lt, time_ms, time_ms + delay, time_ms + delay + d0, 1.0, allow_discrete, bt_tainted, at_tainted);
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
      st = run_leave(w, ri, time_ms, true);
      ri = (size_t)-1;
      res->cancelled++;
    }
    if (st == GTEXT_CSS_OK && di != (size_t)-1) {
      done_remove(w, di);
      res->forgotten++;
    }
  }
  /* 4. a running transition whose end value is not the after-change value */
  if (st == GTEXT_CSS_OK && m >= 0 && ri != (size_t)-1 && !same(w->run[ri].end, w->run[ri].le, at, atl)) {
    tr_t old = w->run[ri];   /* shallow: the strings stay owned by the entry until it is removed */
    /* the "current time" of an old transition an Animation holds is the Animation's (a seek, a pause); for one whose Animation is idle there is no effect and the current value is the end value */
    double old_time;
    const bool old_live = tr_time(&old, time_ms, &old_time);
    st = tr_current_at(context, after, &old, time_ms, al, &cur, &cl);
    bool cur_ok = false;
    if (st == GTEXT_CSS_OK && !same(cur, cl, at, atl)) {
      st = transitionable(context, after, k, cur, cl, at, atl, allow_discrete, &cur_ok);
    }
    if (st == GTEXT_CSS_OK) {
      if (!cur_ok || combined <= 0) {
        /* "if the current value of the property in the running transition is equal to the value of the property in the after-change style, or if these two values are not transitionable,
           then implementations must cancel the running transition", and the same "if the combined duration is less than or equal to 0s" */
        st = run_leave(w, ri, time_ms, true);
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
          if (!old_live || old_time >= old.end_time) {
            progress = 1;
          }
          else if (old_time > old.start_time && old.end_time > old.start_time) {
            progress = (old_time - old.start_time) / (old.end_time - old.start_time);
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
            st = make_tr(w, reg, k, cur, cl, at, atl, rasv, lr, timing, lt, time_ms, start_time, start_time + d0 * rsf, rsf, allow_discrete, old_live ? tr_tainted_at(&old, old_time) : old.end_tainted, at_tainted);
            gtext_allocator_free(al, rasv);
            if (st == GTEXT_CSS_OK) {
              /* the new entry is last; the old one is removed (its index has not moved) */
              st = run_leave(w, ri, time_ms, true);
              res->cancelled++;
              res->started++;
            }
          }
        }
      }
      else {
        st = make_tr(w, reg, k, cur, cl, at, atl, cur, cl, timing, lt, time_ms, time_ms + delay, time_ms + delay + d0, 1.0, allow_discrete, old_live ? tr_tainted_at(&old, old_time) : old.end_tainted, at_tainted);
        if (st == GTEXT_CSS_OK) {
          st = run_leave(w, ri, time_ms, true);
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
  /* "Associated with each top-level browsing context is a current transition generation that is incremented on each style change event" (Transitions 2): one for each of this element's */
  w->generation++;
  /* "Running transitions complete at a time that is equal to or after their end time, but prior to the first style change event whose time is equal to or after their end time. When a
     transition completes, implementations must move all transitions that complete at that time from the set of running transitions to the set of completed transitions" */
  for (size_t i = 0; i < w->nrun && st == GTEXT_CSS_OK;) {
    double over_at;
    if (tr_time(&w->run[i], time_ms, &over_at) && w->run[i].end_time <= over_at) {   /* an Animation that is idle has no time, and does not complete */
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
      st = run_leave(w, i, time_ms, false);
      if (st != GTEXT_CSS_OK) {
        break;
      }
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
  /* the Animations of the transitions this event cancelled get the room for what the cancel makes (the promises that settle, the event it owes) before the set is replaced, so that the cancels
     made after it cannot fail: an update is whole or not at all, and a cancel that was made is not undone */
  for (size_t i = 0; st == GTEXT_CSS_OK && i < w->pc.n; i++) {
    const GTEXT_Anim_Status as = gtext_anim_animation_reserve(w->pc.items[i]);
    st = as == GTEXT_ANIM_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < w->pf.n; i++) {
    const GTEXT_Anim_Status as = gtext_anim_animation_reserve(w->pf.items[i]);
    st = as == GTEXT_ANIM_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_transition_set_free(w);
    return st;
  }
  if (res->started || res->cancelled || res->completed || res->forgotten) {
    set_take(set, w);
    set->version++;
  }
  for (size_t i = 0; i < w->pc.n; i++) {
    (void)gtext_anim_animation_cancel(w->pc.items[i], (GTEXT_Anim_Time){true, time_ms});   /* the room is reserved */
  }
  for (size_t i = 0; i < w->pf.n; i++) {
    /* a transition that completed at a style change event has no record for a frame to ask its Animation whether it finished: it is asked here ("update animations and send events") */
    (void)gtext_anim_animation_update_finished(w->pf.items[i], (GTEXT_Anim_Time){true, time_ms});   /* the room is reserved */
  }
  gtext_css_transition_set_free(w);
  return GTEXT_CSS_OK;
}

/* ---- the events ----------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSS_Transition_Event_Type type;
  uint64_t transition, generation;
  GTEXT_CSS_Property_Id property;
  size_t name_off, name_len;
  double elapsed_ms;
  double scheduled_ms;
} ev_t;

struct GTEXT_CSS_Transition_Events {
  GTEXT_Allocator alloc;
  ev_t * ev;
  size_t n, cap;
  char * pool;
  size_t npool, cappool;
};

GTEXT_CSS_Status gtext_css_transition_events_new(const GTEXT_Allocator * allocator, GTEXT_CSS_Transition_Events ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  GTEXT_Allocator a = allocator ? *allocator : *gtext_allocator_default();
  GTEXT_CSS_Transition_Events * e = gtext_allocator_calloc(&a, 1, sizeof *e);
  if (!e) {
    return GTEXT_CSS_E_OOM;
  }
  e->alloc = a;
  *out = e;
  return GTEXT_CSS_OK;
}

void gtext_css_transition_events_free(GTEXT_CSS_Transition_Events * events) {
  if (!events) {
    return;
  }
  GTEXT_Allocator a = events->alloc;
  gtext_allocator_free(&a, events->ev);
  gtext_allocator_free(&a, events->pool);
  gtext_allocator_free(&a, events);
}

size_t gtext_css_transition_events_count(const GTEXT_CSS_Transition_Events * events) {
  return events ? events->n : 0;
}

bool gtext_css_transition_events_at(const GTEXT_CSS_Transition_Events * events, size_t index, GTEXT_CSS_Transition_Event * out) {
  if (!events || !out || index >= events->n) {
    return false;
  }
  const ev_t * e = &events->ev[index];
  out->type = e->type;
  out->transition = e->transition;
  out->generation = e->generation;
  out->property = e->property;
  out->property_name = (GTEXT_CSS_String){events->pool + e->name_off, e->name_len};
  out->elapsed_time = e->elapsed_ms / 1000.0;
  out->scheduled_time_ms = e->scheduled_ms;
  return true;
}

GTEXT_CSS_Status gtext_css_transition_events_reserve(GTEXT_CSS_Transition_Events * events, size_t more_events, size_t more_name_bytes) {
  if (!events) {
    return GTEXT_CSS_E_INVALID;
  }
  if (more_events > SIZE_MAX / sizeof(ev_t) - events->n || more_name_bytes > SIZE_MAX / 2 - events->npool) {
    return GTEXT_CSS_E_OOM;
  }
  if (events->n + more_events > events->cap) {
    const size_t cap = events->n + more_events;
    ev_t * bigger = gtext_allocator_realloc(&events->alloc, events->ev, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_CSS_E_OOM;
    }
    events->ev = bigger;
    events->cap = cap;
  }
  if (events->npool + more_name_bytes > events->cappool) {
    const size_t cap = events->npool + more_name_bytes;
    char * bigger = gtext_allocator_realloc(&events->alloc, events->pool, cap ? cap : 1);
    if (!bigger) {
      return GTEXT_CSS_E_OOM;
    }
    events->pool = bigger;
    events->cappool = cap;
  }
  return GTEXT_CSS_OK;
}

void gtext_css_transition_set_frame_bound(const GTEXT_CSS_Transition_Set * set, size_t * events, size_t * name_bytes) {
  size_t ne = 0, nb = 0;
  if (set) {
    ne = 3 * (set->nrun + set->ngone);
    for (size_t i = 0; i < set->nrun; i++) {
      nb += set->run[i].lpn;
    }
    for (size_t i = 0; i < set->ngone; i++) {
      nb += set->gone[i].lpn;
    }
  }
  if (events) {
    *events = ne;
  }
  if (name_bytes) {
    *name_bytes = nb;
  }
}

/// The events of one transition moving from one phase to another (Transitions 2, "Event dispatch", the table), with the elapsed times in ms: "interval start" is `max(min(-start delay, active
/// duration), 0)` and "interval end" is `max(min(associated effect end - start delay, active duration), 0)`. Returns how many (0 to 3).
static size_t phase_events(GTEXT_CSS_Transition_Phase from, GTEXT_CSS_Transition_Phase to, double delay, double duration, GTEXT_CSS_Transition_Event_Type type[3], double elapsed[3],
    double ideal[3]) {
  const double s0 = -delay < duration ? -delay : duration;
  const double istart = s0 <= 0 ? 0.0 : s0;
  /* "max(min(associated effect end - start delay, active duration), 0)": the effect ends at the start delay plus the active duration (and no end delay), so it is the active duration; written so
     that no rounding of the sum can make it differ */
  const double iend = duration <= 0 ? 0.0 : duration;
  const GTEXT_CSS_Transition_Event_Type R = GTEXT_CSS_TRANSITION_EVENT_RUN, S = GTEXT_CSS_TRANSITION_EVENT_START, E = GTEXT_CSS_TRANSITION_EVENT_END;
  /* The scheduled event time, relative to the start time of the animation (the style change event that made the transition): "when the event would ideally have been dispatched were animations
     updated at an infinitely high frequency" (Web Animations 1, "Animation events"). The text this library is given does not say the time of each event of a transition (Transitions 2 gives the
     elapsed times only); the ideal time of an event is the moment its phase change happens: the run when the animation starts, the start when the effect's active interval begins (not before
     the animation does), the end when the effect ends. An event of a phase change that goes backwards (only a time that moved back, which a caller that has the model gives a frame) has no
     ideal time, which is -1 here and the time of the frame in the event. */
  const double at_start = delay > 0 ? delay : 0.0;
  const double at_end = delay + duration > 0 ? delay + duration : 0.0;
  size_t n = 0;
#define EV(t, ms, at) (type[n] = (t), elapsed[n] = (ms), ideal[n] = (at), n++)
  if (from == to) {
    return 0;
  }
  switch (from) {
    case GTEXT_CSS_TRANSITION_PHASE_IDLE:
      EV(R, istart, 0.0);
      if (to == GTEXT_CSS_TRANSITION_PHASE_ACTIVE || to == GTEXT_CSS_TRANSITION_PHASE_AFTER) {
        EV(S, istart, at_start);
      }
      if (to == GTEXT_CSS_TRANSITION_PHASE_AFTER) {
        EV(E, iend, at_end);
      }
      break;
    case GTEXT_CSS_TRANSITION_PHASE_BEFORE:
      EV(S, istart, at_start);
      if (to == GTEXT_CSS_TRANSITION_PHASE_AFTER) {
        EV(E, iend, at_end);
      }
      break;
    case GTEXT_CSS_TRANSITION_PHASE_ACTIVE:
      if (to == GTEXT_CSS_TRANSITION_PHASE_AFTER) {
        EV(E, iend, at_end);
      }
      else {
        EV(E, istart, -1.0);
      }
      break;
    case GTEXT_CSS_TRANSITION_PHASE_AFTER:
      EV(S, iend, -1.0);
      if (to == GTEXT_CSS_TRANSITION_PHASE_BEFORE) {
        EV(E, istart, -1.0);
      }
      break;
  }
#undef EV
  return n;
}

/// The scheduled event times of the events of one transition: an ideal time (relative to the start of the animation, or negative for none) is "animation time to timeline time", `base + time x inv`, where
/// `base` is the start time of the animation and `inv` is 1 over its playback rate; an event with no ideal time, or an animation with no start time or a rate of zero, is the frame's.
static void sched_fill(size_t n, const double * ideal, double base, double inv, bool valid, double frame, double * out) {
  for (size_t i = 0; i < n; i++) {
    out[i] = valid && ideal[i] >= 0 ? base + ideal[i] * inv : frame;
  }
}

/// Append the events of one transition (the room is reserved): the name once, the events sharing it.
static void events_add(GTEXT_CSS_Transition_Events * ev, uint64_t id, uint64_t generation, GTEXT_CSS_Property_Id prop, const char * pname, size_t lpn, size_t n,
    const GTEXT_CSS_Transition_Event_Type * type, const double * elapsed, const double * sched) {
  if (!n) {
    return;
  }
  const size_t off = ev->npool;
  if (lpn) {
    memcpy(ev->pool + off, pname, lpn);
  }
  ev->npool += lpn;
  for (size_t i = 0; i < n; i++) {
    ev_t * e = &ev->ev[ev->n++];
    e->type = type[i];
    e->transition = id;
    e->generation = generation;
    e->property = prop;
    e->name_off = off;
    e->name_len = lpn;
    e->elapsed_ms = elapsed[i];
    e->scheduled_ms = sched[i];
  }
}

/// "Within the set of CSS Transitions ... sort A and B in ascending order by the Unicode codepoints that make up the expanded transition property name of each transition" after the transition
/// generation; the order the events of one transition were appended in is kept (a stable insertion sort of the events of one frame, which are nearly in order already).
static bool ev_after(const GTEXT_CSS_Transition_Events * l, const ev_t * a, const ev_t * b) {
  if (a->transition == b->transition) {
    return false;   /* the events of one transition keep the order of the table (their scheduled times do not decrease along it) */
  }
  if (a->scheduled_ms != b->scheduled_ms) {
    return a->scheduled_ms > b->scheduled_ms;   /* "Sort the events by their scheduled event time such that events that were scheduled to occur earlier sort before events scheduled to occur later" */
  }
  if (a->generation != b->generation) {
    return a->generation > b->generation;
  }
  const size_t m = a->name_len < b->name_len ? a->name_len : b->name_len;
  const int c = m ? memcmp(l->pool + a->name_off, l->pool + b->name_off, m) : 0;
  if (c) {
    return c > 0;
  }
  if (a->name_len != b->name_len) {
    return a->name_len > b->name_len;
  }
  return a->transition > b->transition;
}

static void ev_sort(GTEXT_CSS_Transition_Events * l, size_t from) {
  for (size_t i = from + 1; i < l->n; i++) {
    ev_t x = l->ev[i];
    size_t j = i;
    while (j > from && ev_after(l, &l->ev[j - 1], &x)) {
      l->ev[j] = l->ev[j - 1];
      j--;
    }
    l->ev[j] = x;
  }
}

/// Where the copy of the events starts in the scratch of the sort: after the `n` indexes, at the alignment an event needs.
static size_t sort_copy_offset(size_t n) {
  const size_t a = _Alignof(ev_t);
  return (n * sizeof(size_t) + a - 1) / a * a;
}

size_t css_transition_events_sort_bytes(size_t n) {
  return sort_copy_offset(n) + n * sizeof(ev_t);
}

void css_transition_events_sort_by_time(GTEXT_CSS_Transition_Events * l, size_t * from, void * scratch) {
  const size_t n = l ? l->n : 0;
  size_t * tmp = scratch;
  ev_t * copy = (ev_t *)((char *)scratch + sort_copy_offset(n));
  for (size_t i = 0; i < n; i++) {
    from[i] = i;
  }
  /* a bottom-up merge sort of the indexes: stable, n log n, and no allocation */
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      const size_t mid = lo + width < n ? lo + width : n;
      const size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t a = lo, b = mid, k = lo;
      while (a < mid && b < hi) {
        tmp[k++] = l->ev[from[b]].scheduled_ms < l->ev[from[a]].scheduled_ms ? from[b++] : from[a++];
      }
      while (a < mid) {
        tmp[k++] = from[a++];
      }
      while (b < hi) {
        tmp[k++] = from[b++];
      }
    }
    memcpy(from, tmp, n * sizeof *from);
  }
  for (size_t i = 0; i < n; i++) {
    copy[i] = l->ev[from[i]];
  }
  if (n) {
    memcpy(l->ev, copy, n * sizeof *copy);
  }
}

/// One frame. `ready` false is a frame at which the animations are not yet ready to play (their pending play tasks have not run: the first frame that renders them has not completed, or the timeline is
/// inactive): a transition no frame has seen is made *pending*, which "Animation events" and the table of Transitions 2, "Event dispatch", treat as the phase before the effect (idle to pending or
/// before is `transitionrun`, and pending or before to active or after is `transitionstart`), so only the run is reported and the start waits for the first frame that is ready. A transition a ready
/// frame has already seen is not moved by one that is not.
static GTEXT_CSS_Status set_frame(GTEXT_CSS_Transition_Set * set, double time_ms, bool ready, GTEXT_CSS_Transition_Events * events) {
  if (!set || !events || time_ms != time_ms) {
    return GTEXT_CSS_E_INVALID;
  }
  size_t ne, nb;
  gtext_css_transition_set_frame_bound(set, &ne, &nb);
  GTEXT_CSS_Status st = gtext_css_transition_events_reserve(events, ne, nb);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = gtext_css_transition_set_frame_reserve(set);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  /* nothing below can fail, so the set is only changed by a frame that is reported whole */
  const size_t first = events->n;
  GTEXT_CSS_Transition_Event_Type type[3];
  double elapsed[3], ideal[3];
  for (size_t i = 0; i < set->ngone; i++) {
    gone_t * g = &set->gone[i];
    const double duration = g->end_time - g->start_time;
    size_t n;
    if (g->cancelled) {
      type[0] = GTEXT_CSS_TRANSITION_EVENT_CANCEL;
      elapsed[0] = g->elapsed;
      ideal[0] = -1.0;   /* the moment of the cancellation */
      n = 1;
    }
    else {
      n = phase_events(g->phase, GTEXT_CSS_TRANSITION_PHASE_AFTER, g->delay, duration, type, elapsed, ideal);
    }
    double sched[3];
    if (g->held) {
      sched_fill(n, ideal, g->sbase, g->sinv, g->svalid, g->cancelled ? g->left_at : time_ms, sched);
    }
    else {
      sched_fill(n, ideal, g->start_time - g->delay, 1.0, true, g->cancelled ? g->left_at : time_ms, sched);
    }
    events_add(events, g->id, g->generation, g->prop, g->pname, g->lpn, n, type, elapsed, sched);
    gone_free(&set->alloc, g);
  }
  set->ngone = 0;
  for (size_t i = 0; i < set->nrun; i++) {
    tr_t * t = &set->run[i];
    double sched[3];
    if (t->m) {
      /* a transition an Animation holds: the Animation's pending task runs now (a task the style change event made is ready at the time of that event, as for a record that holds its own state; one
         script made is ready at the frame) unless the frame is not ready, it is asked whether it has finished, and the phase is the one its current time and its play state give */
      GTEXT_Anim_Animation_State ms = m_state(t, time_ms);
      if (ready) {
        if (ms.pending) {
          (void)gtext_anim_animation_run_pending(t->m, (GTEXT_Anim_Time){true, t->css_task && ms.ready_generation == t->css_task_gen ? tr_event_time(t) : time_ms});
        }
        t->css_task = false;
        (void)gtext_anim_animation_update_finished(t->m, (GTEXT_Anim_Time){true, time_ms});
        ms = m_state(t, time_ms);
      }
      GTEXT_CSS_Transition_Phase to;
      if (!ms.current_time.resolved) {
        /* Transitions 2, "Event dispatch": "If the transition has an unresolved current time, the transition phase is idle" (an Animation that script cancelled) */
        to = GTEXT_CSS_TRANSITION_PHASE_IDLE;
      }
      else if (!ready) {
        to = t->phase == GTEXT_CSS_TRANSITION_PHASE_IDLE ? GTEXT_CSS_TRANSITION_PHASE_BEFORE : t->phase;
      }
      else if (ms.pending && (t->phase == GTEXT_CSS_TRANSITION_PHASE_IDLE || t->pending_phase)) {
        /* "If the transition has a pending play task or a pending pause task and its phase was previously idle or pending, the transition phase is pending", which the table treats as before */
        to = GTEXT_CSS_TRANSITION_PHASE_BEFORE;
      }
      else {
        /* the phase of the effect at the Animation's current time and rate (Web Animations 1, "Animation effect phases and states": the boundaries of the active interval are the other phase's when the
           animation plays backwards), by the timing model, as the snapshot says it */
        GTEXT_Anim_Effect_Timing tm = tr_timing(t);
        GTEXT_Anim_Computed_Timing ct;
        (void)gtext_anim_computed_timing(&tm, ms.current_time, ms.playback_rate, false, &ct);
        to = ct.phase == GTEXT_ANIM_PHASE_BEFORE ? GTEXT_CSS_TRANSITION_PHASE_BEFORE : ct.phase == GTEXT_ANIM_PHASE_ACTIVE ? GTEXT_CSS_TRANSITION_PHASE_ACTIVE : GTEXT_CSS_TRANSITION_PHASE_AFTER;
      }
      const GTEXT_CSS_Transition_Phase from = t->phase;
      size_t n;
      if (to == GTEXT_CSS_TRANSITION_PHASE_IDLE) {
        /* "not idle and not after -> idle: transitioncancel", with "the active time of the animation at the moment it was canceled calculated using a fill mode of both" (the last the frames saw) */
        n = from != GTEXT_CSS_TRANSITION_PHASE_IDLE && from != GTEXT_CSS_TRANSITION_PHASE_AFTER ? 1 : 0;
        type[0] = GTEXT_CSS_TRANSITION_EVENT_CANCEL;
        elapsed[0] = t->active_both;
        ideal[0] = -1.0;
      }
      else {
        n = phase_events(from, to, t->delay, t->end_time - t->start_time, type, elapsed, ideal);
        t->active_both = tr_active_both(t, ms.current_time, ms.playback_rate);
      }
      double base, inv;
      bool valid;
      tr_sched_basis(t, &ms, &base, &inv, &valid);
      sched_fill(n, ideal, base, inv, valid, time_ms, sched);
      events_add(events, t->id, t->generation, t->prop, t->pname, t->lpn, n, type, elapsed, sched);
      t->pending_phase = to == GTEXT_CSS_TRANSITION_PHASE_BEFORE && ms.pending && (!ready || from == GTEXT_CSS_TRANSITION_PHASE_IDLE || t->pending_phase);
      t->phase = to;
      t->ready = t->ready || (ready && !ms.pending);
      continue;
    }
    const GTEXT_CSS_Transition_Phase to = ready ? phase_at(t->start_time, t->end_time, time_ms) : (t->phase == GTEXT_CSS_TRANSITION_PHASE_IDLE ? GTEXT_CSS_TRANSITION_PHASE_BEFORE : t->phase);
    const size_t n = phase_events(t->phase, to, t->delay, t->end_time - t->start_time, type, elapsed, ideal);
    sched_fill(n, ideal, t->start_time - t->delay, 1.0, true, time_ms, sched);
    events_add(events, t->id, t->generation, t->prop, t->pname, t->lpn, n, type, elapsed, sched);
    t->phase = to;
    t->ready = t->ready || ready;
  }
  ev_sort(events, first);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_transition_set_frame(GTEXT_CSS_Transition_Set * set, double time_ms, GTEXT_CSS_Transition_Events * events) {
  return set_frame(set, time_ms, true, events);
}

GTEXT_CSS_Status gtext_css_transition_set_frame_pending(GTEXT_CSS_Transition_Set * set, double time_ms, GTEXT_CSS_Transition_Events * events) {
  return set_frame(set, time_ms, false, events);
}

GTEXT_CSS_Status gtext_css_transition_set_cancel_all(GTEXT_CSS_Transition_Set * set, double time_ms) {
  if (!set || time_ms != time_ms) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!set->nrun && !set->ndone) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Status st = gone_reserve(set, set->nrun);
  if (st == GTEXT_CSS_OK) {
    st = alist_reserve(&set->alloc, &set->pc, set->nrun);
  }
  if (st == GTEXT_CSS_OK) {
    st = alist_reserve(&set->alloc, &set->pf, set->nrun);
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < set->nrun; i++) {
    if (set->run[i].m && gtext_anim_animation_reserve(set->run[i].m) != GTEXT_ANIM_OK) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  while (set->nrun) {
    /* "Running transitions complete at a time that is equal to or after their end time, but prior to the first style change event whose time is equal to or after their end time": one that is
       over has completed, and reports its end and not a cancellation */
    double at;   /* the time of its own Animation, when an Animation holds it */
    const bool over = tr_time(&set->run[set->nrun - 1], time_ms, &at) && set->run[set->nrun - 1].end_time <= at;
    st = run_leave(set, set->nrun - 1, time_ms, !over);   /* the room is reserved */
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  while (set->ndone) {
    done_remove(set, set->ndone - 1);
  }
  for (size_t i = 0; i < set->pc.n; i++) {
    (void)gtext_anim_animation_cancel(set->pc.items[i], (GTEXT_Anim_Time){true, time_ms});   /* the room is reserved */
  }
  for (size_t i = 0; i < set->pf.n; i++) {
    (void)gtext_anim_animation_update_finished(set->pf.items[i], (GTEXT_Anim_Time){true, time_ms});   /* the room is reserved */
  }
  set->pc.n = 0;
  set->pf.n = 0;
  set->version++;
  return GTEXT_CSS_OK;
}

/* ---- the CSSTransition snapshot ------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_transition_set_animation_at(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Transition_Set * set, size_t index, double time_ms,
    GTEXT_CSS_Transition_Animation * out) {
  if (!context || !set || !out || index >= set->nrun || time_ms != time_ms) {
    return GTEXT_CSS_E_INVALID;
  }
  const tr_t * t = &set->run[index];
  const double duration = t->end_time - t->start_time;
  const double event_time = t->start_time - t->delay;
  memset(out, 0, sizeof *out);
  out->id = t->id;
  out->generation = t->generation;
  out->transition_property = (GTEXT_CSS_String){t->pname, t->lpn};
  out->pending = !t->ready;
  out->start_time_resolved = t->ready;
  out->start_time_ms = event_time;
  /* "the animation's current time": the time since it started, held at 0 while it is pending */
  out->current_time_ms = t->ready ? time_ms - event_time : 0;
  out->current_time_resolved = true;
  if (t->m) {
    /* an Animation holds it: its pending task, start time, current time and play state are the Animation's */
    out->adopted = true;
    out->animation = m_state(t, time_ms);
    out->pending = out->animation.pending;
    out->start_time_resolved = out->animation.start_time.resolved;
    out->start_time_ms = out->animation.start_time.resolved ? out->animation.start_time.ms : 0.0;
    out->current_time_resolved = out->animation.current_time.resolved;
    out->current_time_ms = out->animation.current_time.resolved ? out->animation.current_time.ms : 0.0;
  }
  out->local_time_ms = out->current_time_ms;
  out->delay_ms = t->delay;
  out->end_delay_ms = 0;
  out->duration_ms = duration;
  out->easing = (GTEXT_CSS_String){t->timing, t->lt};
  out->active_duration_ms = duration;
  out->end_time_ms = t->delay + duration;
  out->start_value = (GTEXT_CSS_String){t->start, t->ls};
  out->end_value = (GTEXT_CSS_String){t->end, t->le};
  const double local = out->current_time_ms;
  if (t->m) {
    /* the timing model's answer at the Animation's current time and playback rate (an idle Animation has no local time: no phase, no progress) */
    GTEXT_CSS_Easing * easing = NULL;
    const GTEXT_CSS_Status st = css_easing_from_text(gtext_css_compute_context_registry(context), &set->alloc, t->timing, t->lt, &easing);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    GTEXT_Anim_Effect_Timing tm = tr_timing(t);
    tm.easing = easing;
    GTEXT_Anim_Computed_Timing ct;
    const GTEXT_Anim_Status ts = gtext_anim_computed_timing(&tm, out->animation.current_time, out->animation.playback_rate, false, &ct);
    gtext_css_easing_free(easing);
    if (ts != GTEXT_ANIM_OK) {
      return GTEXT_CSS_E_INVALID;
    }
    out->phase = ct.phase == GTEXT_ANIM_PHASE_BEFORE ? GTEXT_CSS_TRANSITION_PHASE_BEFORE : ct.phase == GTEXT_ANIM_PHASE_ACTIVE ? GTEXT_CSS_TRANSITION_PHASE_ACTIVE :
        ct.phase == GTEXT_ANIM_PHASE_AFTER ? GTEXT_CSS_TRANSITION_PHASE_AFTER : GTEXT_CSS_TRANSITION_PHASE_IDLE;
    out->play_state = out->animation.play_state == GTEXT_ANIM_PLAY_IDLE ? GTEXT_CSS_TRANSITION_PLAY_IDLE : out->animation.play_state == GTEXT_ANIM_PLAY_PAUSED ? GTEXT_CSS_TRANSITION_PLAY_PAUSED :
        out->animation.play_state == GTEXT_ANIM_PLAY_FINISHED ? GTEXT_CSS_TRANSITION_PLAY_FINISHED : GTEXT_CSS_TRANSITION_PLAY_RUNNING;
    out->has_progress = ct.has_progress;
    out->progress = ct.has_progress ? ct.progress : 0.0;
    return GTEXT_CSS_OK;
  }
  out->phase = local < t->delay ? GTEXT_CSS_TRANSITION_PHASE_BEFORE : (local >= out->end_time_ms ? GTEXT_CSS_TRANSITION_PHASE_AFTER : GTEXT_CSS_TRANSITION_PHASE_ACTIVE);
  out->play_state = out->pending || local < out->end_time_ms ? GTEXT_CSS_TRANSITION_PLAY_RUNNING : GTEXT_CSS_TRANSITION_PLAY_FINISHED;
  /* "the active time" with a fill of backwards ("fill of a new transition", the pinned CSSTransition pages): before the effect it is 0 (the start of the first iteration), after it it is unresolved,
     so the progress is null */
  if (out->phase != GTEXT_CSS_TRANSITION_PHASE_AFTER) {
    double input = 0;
    if (out->phase == GTEXT_CSS_TRANSITION_PHASE_ACTIVE) {
      input = (local - t->delay) / duration;
    }
    GTEXT_CSS_Easing * easing = NULL;
    GTEXT_CSS_Status st = css_easing_from_text(gtext_css_compute_context_registry(context), &set->alloc, t->timing, t->lt, &easing);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    out->progress = gtext_css_easing_output(easing, input, out->phase == GTEXT_CSS_TRANSITION_PHASE_BEFORE);
    gtext_css_easing_free(easing);
    out->has_progress = true;
  }
  return GTEXT_CSS_OK;
}

/* ---- the Animation that holds a transition ------------------------------------------------------------------------------------------------------- */

GTEXT_Anim_Status gtext_css_transition_set_bind(GTEXT_CSS_Transition_Set * set, uint64_t id, GTEXT_Anim_Animation * animation, double time_ms) {
  if (!set || !animation || time_ms != time_ms) {
    return GTEXT_ANIM_INVALID;
  }
  tr_t * t = NULL;
  for (size_t i = 0; i < set->nrun; i++) {
    if (set->run[i].id == id) {
      t = &set->run[i];
    }
  }
  if (!t || t->m) {
    return GTEXT_ANIM_INVALID;
  }
  const GTEXT_Anim_Time at = {true, time_ms};
  const GTEXT_Anim_Effect_Timing timing = tr_timing(t);
  GTEXT_Anim_Status st = gtext_anim_animation_set_effect(animation, &timing, at);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  /* what the record had: a transition no ready frame has seen is pending, with its current time held at 0 until the frame makes it ready; any other one started at the style change event that made it */
  GTEXT_Anim_Restore r;
  memset(&r, 0, sizeof r);
  if (t->ready) {
    r.start_time = (GTEXT_Anim_Time){true, tr_event_time(t)};
  }
  else {
    r.hold_time = (GTEXT_Anim_Time){true, 0.0};
    r.pending_play = true;
  }
  st = gtext_anim_animation_restore(animation, &r, at);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  t->m = animation;
  t->css_task = r.pending_play;
  GTEXT_Anim_Animation_State bs;
  (void)gtext_anim_animation_state(animation, at, &bs);
  t->css_task_gen = bs.ready_generation;
  t->pending_phase = !t->ready && t->phase == GTEXT_CSS_TRANSITION_PHASE_BEFORE;
  set->version++;
  return GTEXT_ANIM_OK;
}

void gtext_css_transition_set_detach(GTEXT_CSS_Transition_Set * set, uint64_t id, double time_ms) {
  for (size_t i = 0; set && i < set->nrun; i++) {
    tr_t * t = &set->run[i];
    if (t->id != id || !t->m) {
      continue;
    }
    const GTEXT_Anim_Animation_State ms = m_state(t, time_ms);
    if (!ms.current_time.resolved) {
      /* an idle Animation: the transition has no effect, so it is over (it completes at the next style change event, and tells nothing: it is in the phase `after`) */
      t->start_time = time_ms;
      t->end_time = time_ms;
      t->delay = 0.0;
      t->phase = GTEXT_CSS_TRANSITION_PHASE_AFTER;
      t->ready = true;
    }
    else {
      /* it runs on at the rate of 1 from where the Animation was (a pending task is run: it is ready, with the current time it holds): the record's times move so that its current time at `time_ms` is the
         Animation's */
      const double moved = (time_ms - ms.current_time.ms) - tr_event_time(t);
      t->start_time += moved;
      t->end_time += moved;
      t->ready = true;
    }
    t->m = NULL;
    t->css_task = false;
    set->version++;
  }
}

GTEXT_CSS_Status gtext_css_transition_set_frame_reserve(const GTEXT_CSS_Transition_Set * set) {
  for (size_t i = 0; set && i < set->nrun; i++) {
    /* a frame runs the pending task and then asks whether the Animation finished: two calls, whose room is made first */
    if (set->run[i].m && gtext_anim_animation_reserve_calls(set->run[i].m, 2) != GTEXT_ANIM_OK) {
      return GTEXT_CSS_E_OOM;
    }
  }
  return GTEXT_CSS_OK;
}
