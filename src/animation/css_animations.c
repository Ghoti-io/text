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
 * The CSS Animations of one element (animation/css_animations.h): the records that the style change events make, update and cancel, their snapshot as `CSSAnimation`, and the events of CSS
 * Animations 2, "Event dispatch". The arithmetic of time is the timing model of timing.h; nothing here reads a clock.
 *
 * The animations of a set are kept in the composite order of CSS Animations 2 ("sort A and B based on their position in the computed value of the 'animation-name' property"): every style change
 * event makes the array again, in the order of the list.
 */

#include <ghoti.io/text/animation/css_animations.h>

#include <ghoti.io/text/animation/animation.h>

#include <string.h>

/// The most cancelled animations a set holds for the next frame to report.
#define MAX_OWED 4096
/// The most animations of one element.
#define MAX_ANIMS 4096

typedef struct {
  uint64_t id;
  char * name;
  size_t lname;
  size_t order;                       ///< Its place in the `animation-name` list.
  GTEXT_Anim_Effect_Timing timing;    ///< `timing.easing` is `easing`.
  GTEXT_CSS_Easing * easing;          ///< Owned.
  char * etext;                       ///< The easing, serialized (owned).
  size_t letext;
  unsigned locked;                    ///< The timing members `updateTiming()` took over (GTEXT_ANIM_TIMING_*).
  bool css_paused;                    ///< `animation-play-state` as the last style change event said.
  double event_time;                  ///< The time of the style change event that last played or paused it.
  bool start_valid;
  double start_time;
  bool hold_valid;
  double hold_time;
  uint8_t pending;                    ///< 0: none, 1: a play task, 2: a pause task.
  GTEXT_Anim_Phase phase;             ///< The phase the last frame left it in (idle until a frame sees it).
  double iteration;                   ///< The current iteration of the last frame, when its phase was active.
  GTEXT_Anim_Animation * m;           ///< Borrowed: the Animation that holds its timing state once script has touched it (gtext_anim_css_set_bind()); NULL before. The owner of the set owns it.
  double active_both;                 ///< The active time with a fill of both at the last frame that saw a current time (what an animationcancel reports when the animation is cancelled by script).
  uint32_t css_task_gen;              ///< The ready generation of the Animation when the style change event made that task: a task of a later generation is script's.
  bool css_task;                      ///< The pending task is one a style change event made, which is ready at the time of that event (an adopted animation's own tasks are ready at the frame).
} ca_t;

/// A cancelled animation and what its `animationcancel` owes.
typedef struct {
  char * name;
  size_t lname;
  uint64_t id;
  double elapsed_ms;                  ///< "The active time of the animation at the moment it was cancelled calculated using a fill mode of both".
  double at;                          ///< The moment of the cancellation: its scheduled event time.
} gone_t;

struct GTEXT_Anim_Css_Set {
  GTEXT_Allocator alloc;
  double now;                         ///< The time of the last style change event, frame or binding: what a change made with no time of its own (updateTiming()) is made at.
  ca_t * a;
  size_t n;
  gone_t * gone;
  size_t ngone, capgone;
  uint64_t version;
  uint64_t next_id;
  uint64_t lost;
};

enum { PENDING_NONE = 0, PENDING_PLAY = 1, PENDING_PAUSE = 2 };

/* ---- the owned parts ---------------------------------------------------------------------------------------------------------------------------- */

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

static GTEXT_CSS_Easing * easing_clone(const GTEXT_Allocator * a, const GTEXT_CSS_Easing * src) {
  GTEXT_CSS_Easing * e = gtext_allocator_malloc((GTEXT_Allocator *)a, sizeof *e);
  if (!e) {
    return NULL;
  }
  *e = *src;
  e->allocator_ = *a;
  e->points = NULL;
  if (src->point_count) {
    e->points = gtext_allocator_malloc((GTEXT_Allocator *)a, src->point_count * sizeof *e->points);
    if (!e->points) {
      gtext_allocator_free((GTEXT_Allocator *)a, e);
      return NULL;
    }
    memcpy(e->points, src->points, src->point_count * sizeof *e->points);
  }
  return e;
}

/// The serialization of an easing (`linear` for none), owned.
static char * easing_text(const GTEXT_Allocator * a, const GTEXT_CSS_Easing * e, size_t * length) {
  if (!e) {
    *length = 6;
    return dupn(a, "linear", 6);
  }
  char * text = NULL;
  size_t n = 0;
  if (gtext_css_easing_serialize_buffer(e, a, &text, &n) != GTEXT_CSS_OK) {
    return NULL;
  }
  char * mine = dupn(a, text, n);
  gtext_allocator_free((GTEXT_Allocator *)a, text);
  *length = n;
  return mine;
}

static void ca_release(const GTEXT_Allocator * a, ca_t * c) {
  gtext_allocator_free((GTEXT_Allocator *)a, c->name);
  gtext_css_easing_free(c->easing);
  gtext_allocator_free((GTEXT_Allocator *)a, c->etext);
  memset(c, 0, sizeof *c);
}

static void gone_free(const GTEXT_Allocator * a, gone_t * g) {
  gtext_allocator_free((GTEXT_Allocator *)a, g->name);
  memset(g, 0, sizeof *g);
}

GTEXT_CSS_Status gtext_anim_css_set_new(const GTEXT_Allocator * allocator, GTEXT_Anim_Css_Set ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  GTEXT_Allocator a = allocator ? *allocator : *gtext_allocator_default();
  GTEXT_Anim_Css_Set * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  s->alloc = a;
  *out = s;
  return GTEXT_CSS_OK;
}

void gtext_anim_css_set_clear(GTEXT_Anim_Css_Set * s) {
  if (!s) {
    return;
  }
  for (size_t i = 0; i < s->n; i++) {
    ca_release(&s->alloc, &s->a[i]);
  }
  for (size_t i = 0; i < s->ngone; i++) {
    gone_free(&s->alloc, &s->gone[i]);
  }
  if (s->n || s->ngone) {
    s->version++;
  }
  s->n = 0;
  s->ngone = 0;
}

void gtext_anim_css_set_free(GTEXT_Anim_Css_Set * s) {
  if (!s) {
    return;
  }
  gtext_anim_css_set_clear(s);
  GTEXT_Allocator a = s->alloc;
  gtext_allocator_free(&a, s->a);
  gtext_allocator_free(&a, s->gone);
  gtext_allocator_free(&a, s);
}

uint64_t gtext_anim_css_set_version(const GTEXT_Anim_Css_Set * s) {
  return s ? s->version : 0;
}

uint64_t gtext_anim_css_set_last_id(const GTEXT_Anim_Css_Set * s) {
  return s ? s->next_id : 0;
}

void gtext_anim_css_set_start_ids_after(GTEXT_Anim_Css_Set * s, uint64_t id) {
  if (s && s->next_id < id) {
    s->next_id = id;
  }
}

size_t gtext_anim_css_set_count(const GTEXT_Anim_Css_Set * s) {
  return s ? s->n : 0;
}

uint64_t gtext_anim_css_set_events_lost(const GTEXT_Anim_Css_Set * s) {
  return s ? s->lost : 0;
}

/* ---- the clock of one animation ----------------------------------------------------------------------------------------------------------------- */

/// "The current time of an animation": the held time, or the time since the start time, or unresolved.
static GTEXT_Anim_Time current_of(const ca_t * c, double now) {
  if (c->hold_valid) {
    return (GTEXT_Anim_Time){true, c->hold_time};
  }
  if (c->start_valid) {
    return (GTEXT_Anim_Time){true, now - c->start_time};
  }
  return (GTEXT_Anim_Time){false, 0.0};
}

/// The current time of an animation at a time: the Animation's when script has touched it, else the record's own.
static GTEXT_Anim_Time cur_of(const ca_t * c, double now) {
  if (c->m) {
    GTEXT_Anim_Animation_State ms;
    (void)gtext_anim_animation_state(c->m, (GTEXT_Anim_Time){true, now}, &ms);
    return ms.current_time;
  }
  return current_of(c, now);
}

/// The playback rate that decides the direction of the phases.
static double rate_of(const ca_t * c) {
  if (c->m) {
    GTEXT_Anim_Animation_State ms;
    (void)gtext_anim_animation_state(c->m, (GTEXT_Anim_Time){true, 0.0}, &ms);
    return ms.playback_rate;
  }
  return 1.0;
}

static double infinity(void) {
  volatile double zero = 0.0;
  return 1.0 / zero;
}

/* ---- the style change event --------------------------------------------------------------------------------------------------------------------- */

static GTEXT_Anim_Fill fill_of(GTEXT_CSS_Anim_Fill f) {
  switch (f) {
    case GTEXT_CSS_ANIM_FILL_FORWARDS:
      return GTEXT_ANIM_FILL_FORWARDS;
    case GTEXT_CSS_ANIM_FILL_BACKWARDS:
      return GTEXT_ANIM_FILL_BACKWARDS;
    case GTEXT_CSS_ANIM_FILL_BOTH:
      return GTEXT_ANIM_FILL_BOTH;
    default:
      return GTEXT_ANIM_FILL_NONE;
  }
}

static GTEXT_Anim_Direction direction_of(GTEXT_CSS_Anim_Direction d) {
  switch (d) {
    case GTEXT_CSS_ANIM_DIRECTION_REVERSE:
      return GTEXT_ANIM_DIRECTION_REVERSE;
    case GTEXT_CSS_ANIM_DIRECTION_ALTERNATE:
      return GTEXT_ANIM_DIRECTION_ALTERNATE;
    case GTEXT_CSS_ANIM_DIRECTION_ALTERNATE_REVERSE:
      return GTEXT_ANIM_DIRECTION_ALTERNATE_REVERSE;
    default:
      return GTEXT_ANIM_DIRECTION_NORMAL;
  }
}

/// The timing the `animation-*` properties give an effect, over the members that `updateTiming()` has not taken over (`base` is what the animation had).
static GTEXT_Anim_Effect_Timing timing_from(const GTEXT_CSS_Anim_Effect * e, const GTEXT_Anim_Effect_Timing * base, unsigned locked) {
  GTEXT_Anim_Effect_Timing t = *base;
  if (!(locked & GTEXT_ANIM_TIMING_DELAY)) {
    t.delay = e->delay_start_ms;
  }
  if (!(locked & GTEXT_ANIM_TIMING_END_DELAY)) {
    t.end_delay = e->delay_end_ms;
  }
  if (!(locked & GTEXT_ANIM_TIMING_FILL)) {
    t.fill = fill_of(e->fill);
  }
  if (!(locked & GTEXT_ANIM_TIMING_ITERATIONS)) {
    t.iterations = e->infinite ? infinity() : e->iteration_count;
  }
  if (!(locked & GTEXT_ANIM_TIMING_DURATION)) {
    t.duration_auto = e->duration_auto;
    t.duration = e->duration_auto ? 0.0 : e->duration_ms;
  }
  if (!(locked & GTEXT_ANIM_TIMING_DIRECTION)) {
    t.direction = direction_of(e->direction);
  }
  return t;
}

static bool name_is(const char * a, size_t al, const GTEXT_CSS_String * b) {
  return al == b->length && (al == 0 || memcmp(a, b->data, al) == 0);
}

static bool timing_equal(const GTEXT_Anim_Effect_Timing * a, const GTEXT_Anim_Effect_Timing * b) {
  return a->delay == b->delay && a->end_delay == b->end_delay && a->fill == b->fill && a->iteration_start == b->iteration_start && a->iterations == b->iterations &&
      a->duration_auto == b->duration_auto && a->duration == b->duration && a->direction == b->direction;
}

/// Room for `more` cancelled animations, so that moving them cannot fail: the room is made first and the oldest are dropped (and counted) after, so that a failure changes nothing.
static GTEXT_CSS_Status gone_reserve(GTEXT_Anim_Css_Set * s, size_t more) {
  const size_t want = s->ngone + more > MAX_OWED ? MAX_OWED : s->ngone + more;
  if (want > s->capgone) {
    size_t cap = s->capgone ? s->capgone : 4;
    while (cap < want) {
      cap *= 2;
    }
    gone_t * bigger = gtext_allocator_realloc(&s->alloc, s->gone, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_CSS_E_OOM;
    }
    s->gone = bigger;
    s->capgone = cap;
  }
  while (s->ngone + more > MAX_OWED && s->ngone > 0) {
    gone_free(&s->alloc, &s->gone[0]);
    memmove(&s->gone[0], &s->gone[1], (s->ngone - 1) * sizeof *s->gone);
    s->ngone--;
    s->lost++;
  }
  return GTEXT_CSS_OK;
}

/// "The active time of the animation at the moment it was cancelled calculated using a fill mode of both", in ms.
static double active_time_both(const ca_t * c, double now) {
  GTEXT_Anim_Effect_Timing t = c->timing;
  t.fill = GTEXT_ANIM_FILL_BOTH;
  GTEXT_Anim_Computed_Timing ct;
  if (gtext_anim_computed_timing(&t, cur_of(c, now), rate_of(c), false, &ct) != GTEXT_ANIM_OK || !ct.in_effect) {
    return 0.0;
  }
  return ct.active_time;
}

/// Whether an animation that is cancelled owes an `animationcancel`: "not idle and not after -> idle".
static bool owes_cancel(const ca_t * c) {
  return c->phase != GTEXT_ANIM_PHASE_IDLE && c->phase != GTEXT_ANIM_PHASE_AFTER;
}

/// A planned animation: the record it will be, the index of the animation it updates (or none), and which of its owned parts are new (a failure frees those, a success the old ones).
typedef struct {
  ca_t c;
  size_t from;
  bool fresh_easing, fresh_name, changed;
  int machine_op;                     ///< An adopted animation: 1 pause, 2 play again (animation-play-state changed).
  bool timing_changed;
} plan_t;

static void plan_discard(const GTEXT_Allocator * a, plan_t * p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (p[i].fresh_easing) {
      gtext_css_easing_free(p[i].c.easing);
      gtext_allocator_free((GTEXT_Allocator *)a, p[i].c.etext);
    }
    if (p[i].fresh_name) {
      gtext_allocator_free((GTEXT_Allocator *)a, p[i].c.name);
    }
  }
}

/// Give a planned animation the easing of an effect, if its text differs from the one it has (or it has none): a clone and its text, both new.
static GTEXT_CSS_Status plan_easing(const GTEXT_Allocator * a, plan_t * p, const GTEXT_CSS_Easing * easing, bool have) {
  size_t len = 0;
  char * text = easing_text(a, easing, &len);
  if (!text) {
    return GTEXT_CSS_E_OOM;
  }
  if (have && len == p->c.letext && memcmp(text, p->c.etext, len) == 0) {
    gtext_allocator_free((GTEXT_Allocator *)a, text);
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Easing * clone = easing ? easing_clone(a, easing) : NULL;
  if (easing && !clone) {
    gtext_allocator_free((GTEXT_Allocator *)a, text);
    return GTEXT_CSS_E_OOM;
  }
  p->c.easing = clone;
  p->c.etext = text;
  p->c.letext = len;
  p->fresh_easing = true;
  p->changed = true;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_anim_css_set_update(GTEXT_Anim_Css_Set * s, const GTEXT_CSS_Anim_List * list, const GTEXT_CSS_Atrule_Set * keyframes, double time_ms, GTEXT_Anim_Css_Update * out) {
  if (!s || time_ms != time_ms) {
    return GTEXT_CSS_E_INVALID;
  }
  if (out) {
    memset(out, 0, sizeof *out);
  }
  const GTEXT_Allocator * al = &s->alloc;
  const size_t ne = gtext_css_anim_list_count(list);
  plan_t * plan = gtext_allocator_calloc((GTEXT_Allocator *)al, ne ? ne : 1, sizeof *plan);
  bool * taken = gtext_allocator_calloc((GTEXT_Allocator *)al, s->n ? s->n : 1, sizeof *taken);
  ca_t * next = gtext_allocator_calloc((GTEXT_Allocator *)al, ne ? ne : 1, sizeof *next);
  GTEXT_CSS_Status st = plan && taken && next ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  size_t np = 0, started = 0, updated = 0, cancelled = 0, owed = 0;
  for (size_t i = 0; i < ne && st == GTEXT_CSS_OK && np < MAX_ANIMS; i++) {
    GTEXT_CSS_Anim_Effect e;
    memset(&e, 0, sizeof e);
    if (!gtext_css_anim_list_at(list, i, &e) || !e.timeline_auto) {
      continue;   /* an effect on another timeline is not an animation of the document timeline */
    }
    if (!keyframes || !gtext_css_atrule_set_keyframes(keyframes, e.name.data, e.name.length)) {
      continue;   /* "If there is no @keyframes at-rule with <keyframes-name> matching name ... no animation is generated, and any existing animation matching name is canceled" */
    }
    /* the n-th effect of a name is the n-th animation of it */
    size_t occurrence = 0;
    for (size_t k = 0; k < np; k++) {
      if (name_is(plan[k].c.name, plan[k].c.lname, &e.name)) {
        occurrence++;
      }
    }
    size_t found = (size_t)-1, seen = 0;
    for (size_t k = 0; k < s->n; k++) {
      if (name_is(s->a[k].name, s->a[k].lname, &e.name)) {
        if (seen == occurrence) {
          found = k;
          break;
        }
        seen++;
      }
    }
    plan_t * p = &plan[np];
    p->from = found;
    if (found != (size_t)-1) {
      const ca_t * old = &s->a[found];
      taken[found] = true;
      p->c = *old;
      p->c.timing = timing_from(&e, &old->timing, old->locked);
      if (!(old->locked & GTEXT_ANIM_TIMING_EASING)) {
        st = plan_easing(al, p, e.easing, true);
      }
      p->c.timing.easing = p->c.easing;
      p->timing_changed = !timing_equal(&p->c.timing, &old->timing);
      p->changed = p->changed || p->timing_changed || p->c.order != e.index;
      p->c.order = e.index;
      if (e.paused != old->css_paused && old->m) {
        /* an animation script has touched: the Animation does what the property says (applied at the commit), unless script has played or paused it since ("After a successful call to
           play() or pause() on a CSSAnimation, any subsequent change to the animation-play-state will no longer cause the CSSAnimation to be played or paused") */
        GTEXT_Anim_Animation_State ms;
        (void)gtext_anim_animation_state(old->m, (GTEXT_Anim_Time){true, time_ms}, &ms);
        p->changed = true;
        p->c.css_paused = e.paused;
        p->c.event_time = time_ms;
        p->machine_op = ms.play_state_override ? 0 : (e.paused ? 1 : 2);
      }
      else if (e.paused != old->css_paused) {
        /* "the resolved value of animation-play-state ... is newly paused: pause an animation; newly running: play an animation (auto-rewind false)" */
        p->changed = true;
        p->c.css_paused = e.paused;
        p->c.event_time = time_ms;
        const GTEXT_Anim_Time cur = current_of(old, time_ms);
        if (e.paused) {
          if (cur.resolved) {
            p->c.hold_valid = true;
            p->c.hold_time = cur.ms;
          }
          p->c.pending = PENDING_PAUSE;
        }
        else if (old->pending == PENDING_PAUSE && old->start_valid) {
          p->c.hold_valid = false;   /* the pause had not happened yet: nothing was paused */
          p->c.pending = PENDING_NONE;
        }
        else {
          p->c.pending = PENDING_PLAY;
          p->c.start_valid = false;
        }
        p->c.css_task = p->c.pending != PENDING_NONE;
      }
    }
    else {
      p->c.id = 0;   /* made at the commit: an id is given only to what stays */
      p->c.name = dupn(al, e.name.data, e.name.length);
      p->fresh_name = true;
      if (!p->c.name) {
        st = GTEXT_CSS_E_OOM;
        np++;
        break;
      }
      p->c.lname = e.name.length;
      p->c.order = e.index;
      p->c.timing = timing_from(&e, &(GTEXT_Anim_Effect_Timing){0}, 0);
      p->c.timing.iteration_start = 0;
      p->c.css_paused = e.paused;
      p->c.event_time = time_ms;
      p->c.hold_valid = true;
      p->c.hold_time = 0.0;
      p->c.pending = e.paused ? PENDING_PAUSE : PENDING_PLAY;
      p->c.css_task = true;
      p->c.phase = GTEXT_ANIM_PHASE_IDLE;
      st = plan_easing(al, p, e.easing, false);
      p->c.timing.easing = p->c.easing;
      p->changed = true;
    }
    np++;
  }
  for (size_t k = 0; k < s->n && st == GTEXT_CSS_OK; k++) {
    if (s->a[k].m && gtext_anim_animation_reserve(s->a[k].m) != GTEXT_ANIM_OK) {
      st = GTEXT_CSS_E_OOM;   /* the Animations' notes and events are made after this point, which cannot fail */
    }
    if (st == GTEXT_CSS_OK && !taken[k]) {
      cancelled++;
      owed += owes_cancel(&s->a[k]) ? 1 : 0;
    }
  }
  if (st == GTEXT_CSS_OK) {
    st = gone_reserve(s, owed);
  }
  if (st != GTEXT_CSS_OK) {
    plan_discard(al, plan, np);
    gtext_allocator_free((GTEXT_Allocator *)al, plan);
    gtext_allocator_free((GTEXT_Allocator *)al, taken);
    gtext_allocator_free((GTEXT_Allocator *)al, next);
    return st;
  }
  /* nothing below can fail */
  s->now = time_ms;
  for (size_t i = 0; i < np; i++) {
    /* what the style change event does to an Animation script has touched (a status other than OK leaves it as it was: the model refuses what the specification refuses) */
    if (plan[i].from != (size_t)-1 && plan[i].c.m) {
      const GTEXT_Anim_Time at = {true, time_ms};
      if (plan[i].timing_changed) {
        (void)gtext_anim_animation_set_effect(plan[i].c.m, &plan[i].c.timing, at);
      }
      if (plan[i].machine_op == 1) {
        (void)gtext_anim_animation_css_pause(plan[i].c.m, at);
      }
      else if (plan[i].machine_op == 2) {
        (void)gtext_anim_animation_play_from(plan[i].c.m, at);
      }
      if (plan[i].machine_op) {
        /* a task a style change made is ready at the time of that event: only if the call made one */
        GTEXT_Anim_Animation_State ms;
        (void)gtext_anim_animation_state(plan[i].c.m, at, &ms);
        plan[i].c.css_task = ms.pending;
        plan[i].c.css_task_gen = ms.ready_generation;
      }
    }
  }
  for (size_t k = 0; k < s->n; k++) {
    ca_t * old = &s->a[k];
    if (taken[k]) {
      /* what the planned animation replaced is freed: the old easing when a new one was made (the name is shared) */
      for (size_t i = 0; i < np; i++) {
        if (plan[i].from == k && plan[i].fresh_easing) {
          gtext_css_easing_free(old->easing);
          gtext_allocator_free((GTEXT_Allocator *)al, old->etext);
        }
      }
      continue;
    }
    const double elapsed = owes_cancel(old) ? active_time_both(old, time_ms) : 0.0;   /* before the Animation is cancelled: its current time is the one to read */
    if (old->m) {
      /* "terminate": the Animation is cancelled (its promises settle, its cancel event is owed) and the record lets go of it */
      (void)gtext_anim_animation_cancel(old->m, (GTEXT_Anim_Time){true, time_ms});
      old->m = NULL;
    }
    if (owes_cancel(old)) {
      gone_t * g = &s->gone[s->ngone++];
      g->name = old->name;
      g->lname = old->lname;
      g->id = old->id;
      g->elapsed_ms = elapsed;
      g->at = time_ms;
      old->name = NULL;
    }
    ca_release(al, old);
  }
  for (size_t i = 0; i < np; i++) {
    next[i] = plan[i].c;
    if (plan[i].from == (size_t)-1) {
      next[i].id = ++s->next_id;
      started++;
    }
    else if (plan[i].changed) {
      updated++;
    }
  }
  gtext_allocator_free((GTEXT_Allocator *)al, s->a);
  s->a = next;
  s->n = np;
  gtext_allocator_free((GTEXT_Allocator *)al, plan);
  gtext_allocator_free((GTEXT_Allocator *)al, taken);
  if (started || updated || cancelled) {
    s->version++;
  }
  if (out) {
    out->started = started;
    out->updated = updated;
    out->cancelled = cancelled;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_anim_css_set_cancel_all(GTEXT_Anim_Css_Set * s, double time_ms) {
  if (!s || time_ms != time_ms) {
    return GTEXT_CSS_E_INVALID;
  }
  return gtext_anim_css_set_update(s, NULL, NULL, time_ms, NULL);
}

/* ---- the snapshot ------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_Anim_Play_State play_state_of(const ca_t * c, GTEXT_Anim_Time current, double end_time) {
  if (!current.resolved && c->pending == PENDING_NONE) {
    return GTEXT_ANIM_PLAY_IDLE;
  }
  if (c->pending == PENDING_PAUSE || (!c->start_valid && c->pending != PENDING_PLAY)) {
    return GTEXT_ANIM_PLAY_PAUSED;
  }
  if (current.resolved && current.ms >= end_time) {
    return GTEXT_ANIM_PLAY_FINISHED;
  }
  return GTEXT_ANIM_PLAY_RUNNING;
}

GTEXT_CSS_Status gtext_anim_css_set_info(const GTEXT_Anim_Css_Set * s, size_t index, double time_ms, GTEXT_Anim_Css_Info * out) {
  if (!s || !out || index >= s->n || time_ms != time_ms) {
    return GTEXT_CSS_E_INVALID;
  }
  const ca_t * c = &s->a[index];
  memset(out, 0, sizeof *out);
  out->id = c->id;
  out->animation_name = (GTEXT_CSS_String){c->name, c->lname};
  out->order = c->order;
  out->timing = c->timing;
  out->easing = (GTEXT_CSS_String){c->etext, c->letext};
  out->pending = c->pending != PENDING_NONE;
  /* the start time of an animation with a pending pause is the one it had until it is ready */
  out->start_time = (GTEXT_Anim_Time){c->start_valid && c->pending != PENDING_PLAY, c->start_valid && c->pending != PENDING_PLAY ? c->start_time : 0.0};
  out->current_time = current_of(c, time_ms);
  if (c->m) {
    out->adopted = true;
    (void)gtext_anim_animation_state(c->m, (GTEXT_Anim_Time){true, time_ms}, &out->animation);
    out->pending = out->animation.pending;
    out->start_time = out->animation.start_time;
    out->current_time = out->animation.current_time;
  }
  if (gtext_anim_computed_timing(&c->timing, out->current_time, rate_of(c), false, &out->computed) != GTEXT_ANIM_OK) {
    return GTEXT_CSS_E_INVALID;
  }
  out->play_state = c->m ? out->animation.play_state : play_state_of(c, out->current_time, out->computed.end_time);
  /* "An animation effect is current if ... in the before phase and ... a playback rate > 0", "in play" (active, not finished), or "in effect" */
  out->relevant = out->computed.in_effect || out->computed.phase == GTEXT_ANIM_PHASE_BEFORE || (out->computed.phase == GTEXT_ANIM_PHASE_ACTIVE && out->play_state != GTEXT_ANIM_PLAY_FINISHED);
  return GTEXT_CSS_OK;
}

bool gtext_anim_css_set_current_time(const GTEXT_Anim_Css_Set * s, const GTEXT_CSS_String * name, size_t order, double time_ms, double * ms) {
  if (!s || !name || !ms || time_ms != time_ms) {
    return false;
  }
  for (size_t i = 0; i < s->n; i++) {
    if (s->a[i].order == order && name_is(s->a[i].name, s->a[i].lname, name)) {
      const GTEXT_Anim_Time t = cur_of(&s->a[i], time_ms);
      if (t.resolved) {
        *ms = t.ms;
      }
      return t.resolved;
    }
  }
  return false;
}

GTEXT_Anim_Status gtext_anim_css_set_update_timing(GTEXT_Anim_Css_Set * s, const GTEXT_CSS_Registry * registry, uint64_t id, const GTEXT_Anim_Optional_Timing * input, const char * easing_text_in,
    size_t easing_length) {
  if (!s || !input || !registry) {
    return GTEXT_ANIM_INVALID;
  }
  ca_t * c = NULL;
  for (size_t i = 0; i < s->n; i++) {
    if (s->a[i].id == id) {
      c = &s->a[i];
    }
  }
  if (!c) {
    return GTEXT_ANIM_INVALID;
  }
  GTEXT_CSS_Easing * parsed = NULL;
  char * text = NULL;
  size_t len = 0;
  GTEXT_Anim_Optional_Timing in = *input;
  if (input->present & GTEXT_ANIM_TIMING_EASING) {
    GTEXT_Anim_Status st = gtext_anim_easing_parse(registry, easing_text_in, easing_length, &s->alloc, &parsed);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
    text = easing_text(&s->alloc, parsed, &len);
    if (!text) {
      gtext_css_easing_free(parsed);
      return GTEXT_ANIM_OOM;
    }
    in.values.easing = parsed;
  }
  GTEXT_Anim_Effect_Timing next = c->timing;
  const GTEXT_Anim_Status st = gtext_anim_timing_update(&next, &in);
  if (st != GTEXT_ANIM_OK) {
    gtext_css_easing_free(parsed);
    gtext_allocator_free(&s->alloc, text);
    return st;
  }
  if (input->present & GTEXT_ANIM_TIMING_EASING) {
    gtext_css_easing_free(c->easing);
    gtext_allocator_free(&s->alloc, c->etext);
    c->easing = parsed;
    c->etext = text;
    c->letext = len;
  }
  c->timing = next;
  c->timing.easing = c->easing;
  c->locked |= input->present;
  if (c->m) {
    /* the Animation's effect has the new timing, and the finished state is looked at again ("Updating the timing of an AnimationEffect") */
    (void)gtext_anim_animation_set_effect(c->m, &c->timing, (GTEXT_Anim_Time){true, s->now});
  }
  s->version++;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_css_set_bind(GTEXT_Anim_Css_Set * s, uint64_t id, GTEXT_Anim_Animation * animation, double time_ms) {
  if (!s || !animation || time_ms != time_ms) {
    return GTEXT_ANIM_INVALID;
  }
  ca_t * c = NULL;
  for (size_t i = 0; i < s->n; i++) {
    if (s->a[i].id == id) {
      c = &s->a[i];
    }
  }
  if (!c || c->m) {
    return GTEXT_ANIM_INVALID;
  }
  const GTEXT_Anim_Time at = {true, time_ms};
  GTEXT_Anim_Status st = gtext_anim_animation_set_effect(animation, &c->timing, at);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  GTEXT_Anim_Restore r;
  memset(&r, 0, sizeof r);
  r.hold_time = (GTEXT_Anim_Time){c->hold_valid, c->hold_valid ? c->hold_time : 0.0};
  r.start_time = (GTEXT_Anim_Time){c->start_valid, c->start_valid ? c->start_time : 0.0};
  r.pending_play = c->pending == PENDING_PLAY;
  r.pending_pause = c->pending == PENDING_PAUSE;
  st = gtext_anim_animation_restore(animation, &r, at);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  c->m = animation;
  c->css_task = c->pending != PENDING_NONE;
  GTEXT_Anim_Animation_State bs;
  (void)gtext_anim_animation_state(animation, at, &bs);
  c->css_task_gen = bs.ready_generation;
  c->pending = PENDING_NONE;
  s->now = time_ms;
  s->version++;
  return GTEXT_ANIM_OK;
}

void gtext_anim_css_set_detach(GTEXT_Anim_Css_Set * s, uint64_t id) {
  for (size_t i = 0; s && i < s->n; i++) {
    ca_t * c = &s->a[i];
    if (c->id == id && c->m) {
      /* the record holds its timing state again: what the Animation had at the last time the set saw */
      GTEXT_Anim_Animation_State ms;
      (void)gtext_anim_animation_state(c->m, (GTEXT_Anim_Time){true, s->now}, &ms);
      c->hold_valid = ms.hold_time.resolved;
      c->hold_time = ms.hold_time.resolved ? ms.hold_time.ms : 0.0;
      c->start_valid = ms.start_time.resolved;
      c->start_time = ms.start_time.resolved ? ms.start_time.ms : 0.0;
      c->pending = ms.pending_play ? PENDING_PLAY : ms.pending_pause ? PENDING_PAUSE : PENDING_NONE;
      c->m = NULL;
      s->version++;
    }
  }
}

/* ---- the frame ---------------------------------------------------------------------------------------------------------------------------------- */

void gtext_anim_css_set_frame_bound(const GTEXT_Anim_Css_Set * s, size_t * events, size_t * name_bytes) {
  size_t ne = 0, nb = 0;
  if (s) {
    ne = 2 * s->n + s->ngone;
    for (size_t i = 0; i < s->n; i++) {
      nb += 2 * s->a[i].lname;
    }
    for (size_t i = 0; i < s->ngone; i++) {
      nb += s->gone[i].lname;
    }
  }
  if (events) {
    *events = ne;
  }
  if (name_bytes) {
    *name_bytes = nb;
  }
}

static GTEXT_Anim_Time sched(const ca_t * c, double animation_time, double frame) {
  if (animation_time < 0.0) {
    return (GTEXT_Anim_Time){true, frame};   /* a phase change that goes backwards has no ideal time: the frame's */
  }
  GTEXT_Anim_Time start = {c->start_valid, c->start_time};
  double rate = 1.0;
  if (c->m) {
    GTEXT_Anim_Animation_State ms;
    (void)gtext_anim_animation_state(c->m, (GTEXT_Anim_Time){true, 0.0}, &ms);
    start = ms.start_time;
    rate = ms.playback_rate;
  }
  if (!start.resolved || rate == 0.0 || animation_time >= infinity()) {
    return (GTEXT_Anim_Time){false, 0.0};
  }
  /* "animation time to timeline time": time x (1 / playback rate) + start time */
  return (GTEXT_Anim_Time){true, animation_time * (1.0 / rate) + start.ms};
}

static void emit(GTEXT_Anim_Events * events, const ca_t * c, GTEXT_Anim_Event_Type type, double elapsed_ms, GTEXT_Anim_Time scheduled) {
  GTEXT_Anim_Event ev;
  memset(&ev, 0, sizeof ev);
  ev.type = type;
  ev.animation = c->id;
  ev.name = (GTEXT_CSS_String){c->name, c->lname};
  ev.elapsed_time = elapsed_ms / 1000.0;
  ev.scheduled_time = scheduled;
  (void)gtext_anim_events_append(events, &ev);   /* the room is reserved */
}

GTEXT_CSS_Status gtext_anim_css_set_frame(GTEXT_Anim_Css_Set * s, double time_ms, GTEXT_Anim_Events * events) {
  if (!s || !events || time_ms != time_ms) {
    return GTEXT_CSS_E_INVALID;
  }
  size_t ne, nb;
  gtext_anim_css_set_frame_bound(s, &ne, &nb);
  const GTEXT_Anim_Status rs = gtext_anim_events_reserve(events, ne, nb);
  if (rs != GTEXT_ANIM_OK) {
    return rs == GTEXT_ANIM_OOM ? GTEXT_CSS_E_OOM : GTEXT_CSS_E_INVALID;
  }
  for (size_t i = 0; i < s->n; i++) {
    if (s->a[i].m && gtext_anim_animation_reserve_calls(s->a[i].m, 2) != GTEXT_ANIM_OK) {
      return GTEXT_CSS_E_OOM;   /* the Animations' notes and events are made below (a pending task, then the finished state), which cannot fail */
    }
  }
  /* nothing below can fail, so the set only moves on with a frame that is reported whole */
  s->now = time_ms;
  for (size_t i = 0; i < s->ngone; i++) {
    gone_t * g = &s->gone[i];
    GTEXT_Anim_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = GTEXT_ANIM_EVENT_ANIMATIONCANCEL;
    ev.animation = g->id;
    ev.name = (GTEXT_CSS_String){g->name, g->lname};
    ev.elapsed_time = g->elapsed_ms / 1000.0;
    ev.scheduled_time = (GTEXT_Anim_Time){true, g->at};
    (void)gtext_anim_events_append(events, &ev);
    gone_free(&s->alloc, g);
  }
  s->ngone = 0;
  for (size_t i = 0; i < s->n; i++) {
    ca_t * c = &s->a[i];
    if (c->m) {
      /* an Animation script has touched: its tasks run now (a task a style change event made is ready at the time of that event, as for the records that hold their own state) and it is
         asked whether it has finished */
      GTEXT_Anim_Animation_State ms;
      (void)gtext_anim_animation_state(c->m, (GTEXT_Anim_Time){true, time_ms}, &ms);
      if (ms.pending) {
        (void)gtext_anim_animation_run_pending(c->m, (GTEXT_Anim_Time){true, c->css_task && ms.ready_generation == c->css_task_gen ? c->event_time : time_ms});
      }
      c->css_task = false;
      (void)gtext_anim_animation_update_finished(c->m, (GTEXT_Anim_Time){true, time_ms});
    }
    else if (c->pending == PENDING_PLAY) {
      /* ready: "the start time is set to the ready time less the time held, over the playback rate", the time of the style change event here */
      c->start_valid = true;
      c->start_time = c->event_time - (c->hold_valid ? c->hold_time : 0.0);
      c->hold_valid = false;
      c->pending = PENDING_NONE;
    }
    else if (c->pending == PENDING_PAUSE) {
      c->start_valid = false;
      if (!c->hold_valid) {
        c->hold_valid = true;
        c->hold_time = 0.0;
      }
      c->pending = PENDING_NONE;
    }
    GTEXT_Anim_Computed_Timing ct;
    const GTEXT_Anim_Time cur = cur_of(c, time_ms);
    (void)gtext_anim_computed_timing(&c->timing, cur, rate_of(c), false, &ct);
    const GTEXT_Anim_Phase to = ct.phase, from = c->phase;
    const double delay = c->timing.delay;
    const bool cancelled_by_api = to == GTEXT_ANIM_PHASE_IDLE && from != GTEXT_ANIM_PHASE_IDLE && from != GTEXT_ANIM_PHASE_AFTER;
    if (cancelled_by_api) {
      /* "before or active -> idle: animationcancel", the elapsed time being the active time (fill of both) at the moment it was cancelled: the last the frames saw */
      emit(events, c, GTEXT_ANIM_EVENT_ANIMATIONCANCEL, c->active_both, (GTEXT_Anim_Time){true, time_ms});
    }
    else if (cur.resolved) {
      c->active_both = active_time_both(c, time_ms);
    }
    /* "interval start" and "interval end" of CSS Animations 2, "Event dispatch" */
    double istart = -delay < ct.active_duration ? -delay : ct.active_duration;
    istart = istart > 0 ? istart : 0.0;
    double iend = ct.end_time - delay < ct.active_duration ? ct.end_time - delay : ct.active_duration;
    iend = iend > 0 ? iend : 0.0;
    const double at_start = delay > 0 ? delay : 0.0;
    switch (cancelled_by_api ? GTEXT_ANIM_PHASE_IDLE : from) {
      case GTEXT_ANIM_PHASE_IDLE:
      case GTEXT_ANIM_PHASE_BEFORE:
        if (to == GTEXT_ANIM_PHASE_ACTIVE || to == GTEXT_ANIM_PHASE_AFTER) {
          emit(events, c, GTEXT_ANIM_EVENT_ANIMATIONSTART, istart, sched(c, at_start, time_ms));
        }
        if (to == GTEXT_ANIM_PHASE_AFTER) {
          emit(events, c, GTEXT_ANIM_EVENT_ANIMATIONEND, iend, sched(c, ct.end_time, time_ms));
        }
        break;
      case GTEXT_ANIM_PHASE_ACTIVE:
        if (to == GTEXT_ANIM_PHASE_BEFORE) {
          emit(events, c, GTEXT_ANIM_EVENT_ANIMATIONEND, istart, sched(c, -1.0, time_ms));
        }
        else if (to == GTEXT_ANIM_PHASE_AFTER) {
          emit(events, c, GTEXT_ANIM_EVENT_ANIMATIONEND, iend, sched(c, ct.end_time, time_ms));
        }
        else if (to == GTEXT_ANIM_PHASE_ACTIVE && ct.has_current_iteration && ct.current_iteration != c->iteration) {
          /* "If previous current iteration is greater than current iteration, let iteration boundary be current iteration + 1, otherwise let it be current iteration" */
          const double boundary = c->iteration > ct.current_iteration ? ct.current_iteration + 1.0 : ct.current_iteration;
          const double elapsed = (boundary - c->timing.iteration_start) * ct.timing.duration;
          emit(events, c, GTEXT_ANIM_EVENT_ANIMATIONITERATION, elapsed, sched(c, delay + elapsed, time_ms));
        }
        break;
      case GTEXT_ANIM_PHASE_AFTER:
        if (to == GTEXT_ANIM_PHASE_ACTIVE || to == GTEXT_ANIM_PHASE_BEFORE) {
          emit(events, c, GTEXT_ANIM_EVENT_ANIMATIONSTART, iend, sched(c, -1.0, time_ms));
        }
        if (to == GTEXT_ANIM_PHASE_BEFORE) {
          emit(events, c, GTEXT_ANIM_EVENT_ANIMATIONEND, istart, sched(c, -1.0, time_ms));
        }
        break;
    }
    c->phase = to;
    c->iteration = ct.has_current_iteration ? ct.current_iteration : 0.0;
  }
  s->version++;
  return GTEXT_CSS_OK;
}
