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
 * @file animation.c
 * @brief The Animation state machine of Web Animations 1 (see animation/animation.h). Each static function is the procedure of the section its comment names.
 */

#include <ghoti.io/text/animation/animation.h>

#include <float.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  GTEXT_Anim_Event_Type type;
  GTEXT_Anim_Time current, timeline, scheduled;
} owed_event;

struct GTEXT_Anim_Animation {
  GTEXT_Allocator alloc;
  GTEXT_Anim_Timeline_Kind timeline;
  bool has_effect;
  GTEXT_Anim_Effect_Timing timing;
  double effect_end;
  GTEXT_Anim_Time hold, start, previous_current;
  double rate;
  bool has_pending_rate;
  double pending_rate;
  bool pending_play, pending_pause;
  GTEXT_Anim_Replace_State replace;
  uint32_t ready_generation;
  GTEXT_Anim_Promise_State ready;
  uint32_t finished_generation;
  GTEXT_Anim_Promise_State finished;
  bool finish_microtask;
  GTEXT_Anim_Note * notes;
  size_t n_notes, cap_notes;
  owed_event * events;
  size_t n_events, cap_events;
};

static const GTEXT_Anim_Time UNRESOLVED = {false, 0.0};

/* An infinite effect end, without <math.h>. */
static bool is_infinite(double x) {
  return x > DBL_MAX;
}

static GTEXT_Anim_Time at(double ms) {
  GTEXT_Anim_Time t = {true, ms};
  return t;
}

/* A timeline is active when there is one and its current time is resolved. */
static bool active(const GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  return a->timeline != GTEXT_ANIM_TIMELINE_NONE && now.resolved;
}

/* "The timeline's current time", or unresolved for no timeline or an inactive one. */
static GTEXT_Anim_Time timeline_time(const GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  return active(a, now) ? at(now.ms) : UNRESOLVED;
}

static bool finite_timeline(const GTEXT_Anim_Animation * a) {
  return a->timeline == GTEXT_ANIM_TIMELINE_PROGRESS;
}

static double effective_rate(const GTEXT_Anim_Animation * a) {
  return a->has_pending_rate ? a->pending_rate : a->rate;
}

/* "Calculating the current time of an animation". `use_hold` false substitutes an unresolved hold time (the "unconstrained current time"). */
static GTEXT_Anim_Time current_time(const GTEXT_Anim_Animation * a, GTEXT_Anim_Time now, bool use_hold) {
  if (use_hold && a->hold.resolved) {
    return a->hold;
  }
  if (!active(a, now) || !a->start.resolved) {
    return UNRESOLVED;
  }
  return at((now.ms - a->start.ms) * a->rate);
}

/* "Associated effect end": the end time of the associated effect, zero with none. */
static double effect_end(const GTEXT_Anim_Animation * a) {
  return a->has_effect ? a->effect_end : 0.0;
}

/* "Play states". */
static GTEXT_Anim_Play_State play_state(const GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  const GTEXT_Anim_Time cur = current_time(a, now, true);
  if (!cur.resolved && !a->start.resolved && !a->pending_play && !a->pending_pause) {
    return GTEXT_ANIM_PLAY_IDLE;
  }
  if (a->pending_pause || (!a->start.resolved && !a->pending_play)) {
    return GTEXT_ANIM_PLAY_PAUSED;
  }
  const double r = effective_rate(a);
  if (cur.resolved && ((r > 0 && cur.ms >= effect_end(a)) || (r < 0 && cur.ms <= 0))) {
    return GTEXT_ANIM_PLAY_FINISHED;
  }
  return GTEXT_ANIM_PLAY_RUNNING;
}

static GTEXT_Anim_Status push_note(GTEXT_Anim_Animation * a, GTEXT_Anim_Note_Kind kind, uint32_t generation) {
  if (a->n_notes == a->cap_notes) {
    const size_t cap = a->cap_notes ? a->cap_notes * 2 : 8;
    GTEXT_Anim_Note * bigger = gtext_allocator_realloc(&a->alloc, a->notes, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_ANIM_OOM;
    }
    a->notes = bigger;
    a->cap_notes = cap;
  }
  a->notes[a->n_notes].kind = kind;
  a->notes[a->n_notes].generation = generation;
  a->n_notes++;
  return GTEXT_ANIM_OK;
}

static GTEXT_Anim_Status push_event(GTEXT_Anim_Animation * a, GTEXT_Anim_Event_Type type, GTEXT_Anim_Time current, GTEXT_Anim_Time timeline, GTEXT_Anim_Time scheduled) {
  if (a->n_events == a->cap_events) {
    const size_t cap = a->cap_events ? a->cap_events * 2 : 8;
    owed_event * bigger = gtext_allocator_realloc(&a->alloc, a->events, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_ANIM_OOM;
    }
    a->events = bigger;
    a->cap_events = cap;
  }
  a->events[a->n_events].type = type;
  a->events[a->n_events].current = current;
  a->events[a->n_events].timeline = timeline;
  a->events[a->n_events].scheduled = scheduled;
  a->n_events++;
  return GTEXT_ANIM_OK;
}

/* The model keeps its notes and events in memory it has to grow. A failure to grow is reported by the call that made the note, after the state moved: the specification has no step that can
 * fail here, so the model reserves room for the largest number of notes and events one call can make (four) before it changes anything. */
static GTEXT_Anim_Status reserve_room(GTEXT_Anim_Animation * a) {
  while (a->cap_notes - a->n_notes < 4 || a->cap_events - a->n_events < 2) {
    if (a->cap_notes - a->n_notes < 4) {
      const size_t cap = a->cap_notes ? a->cap_notes * 2 : 8;
      GTEXT_Anim_Note * bigger = gtext_allocator_realloc(&a->alloc, a->notes, cap * sizeof *bigger);
      if (!bigger) {
        return GTEXT_ANIM_OOM;
      }
      a->notes = bigger;
      a->cap_notes = cap;
    }
    if (a->cap_events - a->n_events < 2) {
      const size_t cap = a->cap_events ? a->cap_events * 2 : 4;
      owed_event * bigger = gtext_allocator_realloc(&a->alloc, a->events, cap * sizeof *bigger);
      if (!bigger) {
        return GTEXT_ANIM_OOM;
      }
      a->events = bigger;
      a->cap_events = cap;
    }
  }
  return GTEXT_ANIM_OK;
}

/* "Resolve a Promise" on the current ready promise: a settled promise does not change. */
static void resolve_ready(GTEXT_Anim_Animation * a) {
  if (a->ready == GTEXT_ANIM_PROMISE_PENDING) {
    a->ready = GTEXT_ANIM_PROMISE_RESOLVED;
    (void)push_note(a, GTEXT_ANIM_NOTE_READY_RESOLVE, a->ready_generation);
  }
}

/* "Let the current ready promise be a new promise" and the new resolved promise of "reset an animation's pending tasks". */
static void new_ready_promise(GTEXT_Anim_Animation * a, GTEXT_Anim_Promise_State state) {
  a->ready_generation++;
  a->ready = state;
}

/* "Apply any pending playback rate". */
static void apply_pending_rate(GTEXT_Anim_Animation * a) {
  if (a->has_pending_rate) {
    a->rate = a->pending_rate;
    a->has_pending_rate = false;
  }
}

/* The scheduled event time of the finish event: "animation time to origin-relative time" of the associated effect end. The document timeline's origin-relative time is its own time. */
static GTEXT_Anim_Time finish_scheduled_time(const GTEXT_Anim_Animation * a) {
  const double end = effect_end(a);
  if (!a->start.resolved || a->rate == 0.0) {
    /* "If timeline time is unresolved, return time": the animation time itself */
    return at(end);
  }
  return at(end * (1.0 / a->rate) + a->start.ms);
}

/* "Finish notification steps". */
static void finish_notification_steps(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  if (play_state(a, now) != GTEXT_ANIM_PLAY_FINISHED) {
    return;
  }
  if (a->finished == GTEXT_ANIM_PROMISE_PENDING) {
    a->finished = GTEXT_ANIM_PROMISE_RESOLVED;
    (void)push_note(a, GTEXT_ANIM_NOTE_FINISHED_RESOLVE, a->finished_generation);
  }
  /* an animation with no timeline has no document for timing: the event is a task of its own, which has no scheduled time */
  const GTEXT_Anim_Time sched = a->timeline != GTEXT_ANIM_TIMELINE_NONE ? finish_scheduled_time(a) : UNRESOLVED;
  (void)push_event(a, GTEXT_ANIM_EVENT_FINISH, current_time(a, now, true), timeline_time(a, now), sched);
}

/* "Update an animation's finished state". */
static void update_finished(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now, bool did_seek, bool sync_notify) {
  const GTEXT_Anim_Time unconstrained = current_time(a, now, did_seek);
  if (unconstrained.resolved && a->start.resolved && !a->pending_play && !a->pending_pause) {
    const double end = effect_end(a);
    if (a->rate > 0 && unconstrained.ms >= end) {
      if (did_seek) {
        a->hold = unconstrained;
      } else if (a->previous_current.resolved) {
        a->hold = at(a->previous_current.ms > end ? a->previous_current.ms : end);
      } else {
        a->hold = at(end);
      }
    } else if (a->rate < 0 && unconstrained.ms <= 0) {
      if (did_seek) {
        a->hold = unconstrained;
      } else if (a->previous_current.resolved) {
        a->hold = at(a->previous_current.ms < 0 ? a->previous_current.ms : 0.0);
      } else {
        a->hold = at(0.0);
      }
    } else if (a->rate != 0 && active(a, now)) {
      if (did_seek && a->hold.resolved) {
        a->start = at(now.ms - a->hold.ms / a->rate);
      }
      a->hold = UNRESOLVED;
    }
  }
  a->previous_current = current_time(a, now, true);
  const bool finished_now = play_state(a, now) == GTEXT_ANIM_PLAY_FINISHED;
  if (finished_now && a->finished == GTEXT_ANIM_PROMISE_PENDING) {
    if (sync_notify) {
      a->finish_microtask = false;
      finish_notification_steps(a, now);
    } else {
      a->finish_microtask = true;
    }
  }
  if (!finished_now && a->finished == GTEXT_ANIM_PROMISE_RESOLVED) {
    a->finished_generation++;
    a->finished = GTEXT_ANIM_PROMISE_PENDING;
  }
}

/* "Silently set the current time". */
static GTEXT_Anim_Status silently_set_current_time(GTEXT_Anim_Animation * a, GTEXT_Anim_Time seek, GTEXT_Anim_Time now) {
  if (!seek.resolved) {
    return current_time(a, now, true).resolved ? GTEXT_ANIM_TYPE_ERROR : GTEXT_ANIM_OK;
  }
  if (a->hold.resolved || !a->start.resolved || !active(a, now) || a->rate == 0.0) {
    a->hold = seek;
  } else {
    a->start = at(now.ms - seek.ms / a->rate);
  }
  if (!active(a, now)) {
    a->start = UNRESOLVED;
  }
  a->previous_current = UNRESOLVED;
  return GTEXT_ANIM_OK;
}

/* "Set the current time". */
static GTEXT_Anim_Status do_set_current_time(GTEXT_Anim_Animation * a, GTEXT_Anim_Time seek, GTEXT_Anim_Time now) {
  GTEXT_Anim_Status st = silently_set_current_time(a, seek, now);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  if (a->pending_pause) {
    a->hold = seek;
    apply_pending_rate(a);
    a->start = UNRESOLVED;
    a->pending_pause = false;
    resolve_ready(a);
  }
  update_finished(a, now, true, false);
  return GTEXT_ANIM_OK;
}

/* "Play an animation". */
static GTEXT_Anim_Status do_play(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now, bool auto_rewind) {
  const bool aborted_pause = a->pending_pause;
  bool has_pending_ready_promise = false;
  GTEXT_Anim_Time seek = UNRESOLVED;
  const GTEXT_Anim_Time cur = current_time(a, now, true);
  const double r = effective_rate(a);
  const double end = effect_end(a);
  if (auto_rewind) {
    if (r >= 0 && (!cur.resolved || cur.ms < 0 || cur.ms >= end)) {
      seek = at(0.0);
    } else if (r < 0 && (!cur.resolved || cur.ms <= 0 || cur.ms > end)) {
      if (is_infinite(end)) {
        return GTEXT_ANIM_INVALID_STATE;
      }
      seek = at(end);
    }
  }
  if (!seek.resolved && !a->start.resolved && !cur.resolved) {
    seek = at(0.0);
  }
  if (reserve_room(a) != GTEXT_ANIM_OK) {
    return GTEXT_ANIM_OOM;
  }
  if (seek.resolved) {
    if (finite_timeline(a)) {
      a->start = seek;
      a->hold = UNRESOLVED;
      apply_pending_rate(a);
    } else {
      a->hold = seek;
    }
  }
  if (a->hold.resolved) {
    a->start = UNRESOLVED;
  }
  if (a->pending_play || a->pending_pause) {
    a->pending_play = false;
    a->pending_pause = false;
    has_pending_ready_promise = true;
  }
  if (!a->hold.resolved && !seek.resolved && !aborted_pause && !a->has_pending_rate) {
    return GTEXT_ANIM_OK;
  }
  if (!has_pending_ready_promise) {
    new_ready_promise(a, GTEXT_ANIM_PROMISE_PENDING);
  }
  a->pending_play = true;
  update_finished(a, now, false, false);
  return GTEXT_ANIM_OK;
}

/* "Pause an animation". */
static GTEXT_Anim_Status do_pause(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  if (a->pending_pause || play_state(a, now) == GTEXT_ANIM_PLAY_PAUSED) {
    return GTEXT_ANIM_OK;
  }
  GTEXT_Anim_Time seek = UNRESOLVED;
  if (!current_time(a, now, true).resolved) {
    if (a->rate >= 0) {
      seek = at(0.0);
    } else if (is_infinite(effect_end(a))) {
      return GTEXT_ANIM_INVALID_STATE;
    } else {
      seek = at(effect_end(a));
    }
  }
  if (reserve_room(a) != GTEXT_ANIM_OK) {
    return GTEXT_ANIM_OOM;
  }
  if (seek.resolved) {
    if (finite_timeline(a)) {
      a->start = seek;
    } else {
      a->hold = seek;
    }
  }
  bool has_pending_ready_promise = false;
  if (a->pending_play) {
    a->pending_play = false;
    has_pending_ready_promise = true;
  }
  if (!has_pending_ready_promise) {
    new_ready_promise(a, GTEXT_ANIM_PROMISE_PENDING);
  }
  a->pending_pause = true;
  update_finished(a, now, false, false);
  return GTEXT_ANIM_OK;
}

/* "Reset an animation's pending tasks". */
static void reset_pending_tasks(GTEXT_Anim_Animation * a) {
  if (!a->pending_play && !a->pending_pause) {
    return;
  }
  a->pending_play = false;
  a->pending_pause = false;
  apply_pending_rate(a);
  if (a->ready == GTEXT_ANIM_PROMISE_PENDING) {
    a->ready = GTEXT_ANIM_PROMISE_REJECTED;
    (void)push_note(a, GTEXT_ANIM_NOTE_READY_REJECT, a->ready_generation);
  }
  new_ready_promise(a, GTEXT_ANIM_PROMISE_RESOLVED);
}

/* "Cancel an animation". */
static void do_cancel(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  if (play_state(a, now) != GTEXT_ANIM_PLAY_IDLE) {
    reset_pending_tasks(a);
    if (a->finished == GTEXT_ANIM_PROMISE_PENDING) {
      a->finished = GTEXT_ANIM_PROMISE_REJECTED;
      (void)push_note(a, GTEXT_ANIM_NOTE_FINISHED_REJECT, a->finished_generation);
    }
    a->finished_generation++;
    a->finished = GTEXT_ANIM_PROMISE_PENDING;
    const GTEXT_Anim_Time tt = timeline_time(a, now);
    /* with a document timeline the event waits in the queue of the document, scheduled at the timeline time; a progress-based timeline has no conversion to an origin-relative time */
    const GTEXT_Anim_Time sched = a->timeline == GTEXT_ANIM_TIMELINE_DOCUMENT ? tt : UNRESOLVED;
    (void)push_event(a, GTEXT_ANIM_EVENT_CANCEL, UNRESOLVED, tt, sched);
  }
  a->hold = UNRESOLVED;
  a->start = UNRESOLVED;
}

/* "Finish an animation". */
static GTEXT_Anim_Status do_finish(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  const double r = effective_rate(a);
  if (r == 0.0 || (r > 0 && is_infinite(effect_end(a)))) {
    return GTEXT_ANIM_INVALID_STATE;
  }
  if (reserve_room(a) != GTEXT_ANIM_OK) {
    return GTEXT_ANIM_OOM;
  }
  const bool was_idle = play_state(a, now) == GTEXT_ANIM_PLAY_IDLE;
  apply_pending_rate(a);
  const double limit = a->rate > 0 ? effect_end(a) : 0.0;
  (void)silently_set_current_time(a, at(limit), now);
  if (!a->start.resolved && active(a, now)) {
    a->start = at(now.ms - limit / a->rate);
  }
  if (a->pending_pause && a->start.resolved) {
    /* "Let the hold time be unresolved, except in the case when the animation was previously idle" */
    if (!was_idle) {
      a->hold = UNRESOLVED;
    }
    a->pending_pause = false;
    resolve_ready(a);
  }
  if (a->pending_play && a->start.resolved) {
    a->pending_play = false;
    resolve_ready(a);
  }
  update_finished(a, now, true, true);
  return GTEXT_ANIM_OK;
}

/* "Set the playback rate". */
static void do_set_playback_rate(GTEXT_Anim_Animation * a, double rate, GTEXT_Anim_Time now) {
  a->has_pending_rate = false;
  const GTEXT_Anim_Time previous_time = current_time(a, now, true);
  const double previous_rate = effective_rate(a);
  a->rate = rate;
  if (a->timeline == GTEXT_ANIM_TIMELINE_DOCUMENT && previous_time.resolved) {
    (void)do_set_current_time(a, previous_time, now);
  } else if (a->timeline != GTEXT_ANIM_TIMELINE_NONE && a->timeline != GTEXT_ANIM_TIMELINE_DOCUMENT && a->start.resolved && !is_infinite(effect_end(a)) &&
             ((previous_rate < 0 && rate >= 0) || (previous_rate >= 0 && rate < 0))) {
    a->start = at(effect_end(a) - a->start.ms);
  }
  update_finished(a, now, true, false);
}

/* "Seamlessly update the playback rate". */
static GTEXT_Anim_Status do_update_playback_rate(GTEXT_Anim_Animation * a, double rate, GTEXT_Anim_Time now) {
  const GTEXT_Anim_Play_State previous = play_state(a, now);
  a->has_pending_rate = true;
  a->pending_rate = rate;
  if (a->pending_play || a->pending_pause) {
    return GTEXT_ANIM_OK;
  }
  if (previous == GTEXT_ANIM_PLAY_IDLE || previous == GTEXT_ANIM_PLAY_PAUSED || !current_time(a, now, true).resolved) {
    apply_pending_rate(a);
    return GTEXT_ANIM_OK;
  }
  if (previous == GTEXT_ANIM_PLAY_FINISHED) {
    const GTEXT_Anim_Time unconstrained = current_time(a, now, false);
    if (active(a, now) && unconstrained.resolved) {
      a->start = a->pending_rate == 0.0 ? at(now.ms) : at(now.ms - unconstrained.ms / a->pending_rate);
    }
    apply_pending_rate(a);
    update_finished(a, now, false, false);
    return GTEXT_ANIM_OK;
  }
  return do_play(a, now, false);
}

/* The public entry points reserve room for the notes and events of one call before they change anything. */
#define ENTER(a) \
  do { \
    if (!(a)) { \
      return GTEXT_ANIM_INVALID; \
    } \
    if (reserve_room(a) != GTEXT_ANIM_OK) { \
      return GTEXT_ANIM_OOM; \
    } \
  } while (0)

GTEXT_Anim_Status gtext_anim_animation_new(const GTEXT_Allocator * allocator, GTEXT_Anim_Timeline_Kind timeline, GTEXT_Anim_Animation ** out) {
  if (!out || (timeline != GTEXT_ANIM_TIMELINE_NONE && timeline != GTEXT_ANIM_TIMELINE_DOCUMENT && timeline != GTEXT_ANIM_TIMELINE_PROGRESS)) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  GTEXT_Allocator al = allocator ? *allocator : *gtext_allocator_default();
  GTEXT_Anim_Animation * a = gtext_allocator_calloc(&al, 1, sizeof *a);
  if (!a) {
    return GTEXT_ANIM_OOM;
  }
  a->alloc = al;
  a->timeline = timeline;
  a->rate = 1.0;
  a->hold = UNRESOLVED;
  a->start = UNRESOLVED;
  a->previous_current = UNRESOLVED;
  a->replace = GTEXT_ANIM_REPLACE_ACTIVE;
  a->ready_generation = 1;
  a->ready = GTEXT_ANIM_PROMISE_RESOLVED;
  a->finished_generation = 1;
  a->finished = GTEXT_ANIM_PROMISE_PENDING;
  *out = a;
  return GTEXT_ANIM_OK;
}

void gtext_anim_animation_free(GTEXT_Anim_Animation * animation) {
  if (!animation) {
    return;
  }
  GTEXT_Allocator al = animation->alloc;
  gtext_allocator_free(&al, animation->notes);
  gtext_allocator_free(&al, animation->events);
  gtext_allocator_free(&al, animation);
}

GTEXT_Anim_Status gtext_anim_animation_set_timeline(GTEXT_Anim_Animation * a, GTEXT_Anim_Timeline_Kind kind, GTEXT_Anim_Time now) {
  ENTER(a);
  if (kind != GTEXT_ANIM_TIMELINE_NONE && kind != GTEXT_ANIM_TIMELINE_DOCUMENT && kind != GTEXT_ANIM_TIMELINE_PROGRESS) {
    return GTEXT_ANIM_INVALID;
  }
  if (kind == a->timeline) {
    return GTEXT_ANIM_OK;
  }
  if (kind == GTEXT_ANIM_TIMELINE_PROGRESS || a->timeline == GTEXT_ANIM_TIMELINE_PROGRESS) {
    return GTEXT_ANIM_NOT_SUPPORTED;
  }
  a->timeline = kind;
  if (a->start.resolved) {
    a->hold = UNRESOLVED;
  }
  update_finished(a, now, false, false);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_set_effect(GTEXT_Anim_Animation * a, const GTEXT_Anim_Effect_Timing * timing, GTEXT_Anim_Time now) {
  ENTER(a);
  if (timing) {
    GTEXT_Anim_Status st = gtext_anim_timing_validate(timing);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
    GTEXT_Anim_Computed_Timing c;
    st = gtext_anim_computed_timing(timing, UNRESOLVED, 1.0, false, &c);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
    a->timing = *timing;
    a->effect_end = c.end_time;
    a->has_effect = true;
  } else {
    a->has_effect = false;
    a->effect_end = 0.0;
  }
  update_finished(a, now, false, false);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_update_effect_timing(GTEXT_Anim_Animation * a, const GTEXT_Anim_Optional_Timing * input, GTEXT_Anim_Time now) {
  ENTER(a);
  if (!input || !a->has_effect) {
    return GTEXT_ANIM_INVALID;
  }
  GTEXT_Anim_Effect_Timing next = a->timing;
  GTEXT_Anim_Status st = gtext_anim_timing_update(&next, input);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  GTEXT_Anim_Computed_Timing c;
  st = gtext_anim_computed_timing(&next, UNRESOLVED, 1.0, false, &c);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  a->timing = next;
  a->effect_end = c.end_time;
  update_finished(a, now, false, false);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_state(const GTEXT_Anim_Animation * a, GTEXT_Anim_Time now, GTEXT_Anim_Animation_State * out) {
  if (!a || !out) {
    return GTEXT_ANIM_INVALID;
  }
  memset(out, 0, sizeof *out);
  out->current_time = current_time(a, now, true);
  out->start_time = a->start;
  out->hold_time = a->hold;
  out->play_state = play_state(a, now);
  out->replace_state = a->replace;
  out->pending = a->pending_play || a->pending_pause;
  out->pending_play = a->pending_play;
  out->pending_pause = a->pending_pause;
  out->playback_rate = a->rate;
  out->has_pending_rate = a->has_pending_rate;
  out->pending_rate = a->pending_rate;
  out->effective_rate = effective_rate(a);
  out->effect_end = effect_end(a);
  out->has_effect = a->has_effect;
  out->timeline = a->timeline;
  out->ready_generation = a->ready_generation;
  out->ready = a->ready;
  out->finished_generation = a->finished_generation;
  out->finished = a->finished;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_set_current_time(GTEXT_Anim_Animation * a, GTEXT_Anim_Time seek, GTEXT_Anim_Time now) {
  ENTER(a);
  return do_set_current_time(a, seek, now);
}

GTEXT_Anim_Status gtext_anim_animation_set_start_time(GTEXT_Anim_Animation * a, GTEXT_Anim_Time start, GTEXT_Anim_Time now) {
  ENTER(a);
  /* "Set the start time" */
  if (!active(a, now) && start.resolved) {
    a->hold = UNRESOLVED;
  }
  const GTEXT_Anim_Time previous = current_time(a, now, true);
  apply_pending_rate(a);
  a->start = start;
  if (start.resolved) {
    if (a->rate != 0.0) {
      a->hold = UNRESOLVED;
    }
  } else {
    a->hold = previous;
  }
  if (a->pending_play || a->pending_pause) {
    a->pending_play = false;
    a->pending_pause = false;
    resolve_ready(a);
  }
  update_finished(a, now, true, false);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_play(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  ENTER(a);
  return do_play(a, now, true);
}

GTEXT_Anim_Status gtext_anim_animation_pause(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  ENTER(a);
  return do_pause(a, now);
}

GTEXT_Anim_Status gtext_anim_animation_finish(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  ENTER(a);
  return do_finish(a, now);
}

GTEXT_Anim_Status gtext_anim_animation_cancel(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  ENTER(a);
  do_cancel(a, now);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_reverse(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  ENTER(a);
  /* "Reverse an animation" */
  if (!active(a, now)) {
    return GTEXT_ANIM_INVALID_STATE;
  }
  const bool original_has = a->has_pending_rate;
  const double original = a->pending_rate;
  const double inverse = -effective_rate(a);
  a->has_pending_rate = true;
  a->pending_rate = inverse;
  GTEXT_Anim_Status st = do_play(a, now, true);
  if (st != GTEXT_ANIM_OK) {
    a->has_pending_rate = original_has;
    a->pending_rate = original;
  }
  return st;
}

GTEXT_Anim_Status gtext_anim_animation_set_playback_rate(GTEXT_Anim_Animation * a, double rate, GTEXT_Anim_Time now) {
  ENTER(a);
  if (!(rate == rate) || rate > DBL_MAX || rate < -DBL_MAX) {
    return GTEXT_ANIM_TYPE_ERROR;
  }
  do_set_playback_rate(a, rate, now);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_update_playback_rate(GTEXT_Anim_Animation * a, double rate, GTEXT_Anim_Time now) {
  ENTER(a);
  if (!(rate == rate) || rate > DBL_MAX || rate < -DBL_MAX) {
    return GTEXT_ANIM_TYPE_ERROR;
  }
  return do_update_playback_rate(a, rate, now);
}

GTEXT_Anim_Status gtext_anim_animation_persist(GTEXT_Anim_Animation * a) {
  if (!a) {
    return GTEXT_ANIM_INVALID;
  }
  a->replace = GTEXT_ANIM_REPLACE_PERSISTED;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_remove_replaced(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now, bool * removed) {
  ENTER(a);
  if (removed) {
    *removed = false;
  }
  if (play_state(a, now) != GTEXT_ANIM_PLAY_FINISHED || a->replace == GTEXT_ANIM_REPLACE_REMOVED || a->timeline != GTEXT_ANIM_TIMELINE_DOCUMENT || !a->has_effect) {
    return GTEXT_ANIM_OK;
  }
  /* "in effect": the active time of the effect is resolved */
  GTEXT_Anim_Computed_Timing c;
  GTEXT_Anim_Status st = gtext_anim_computed_timing(&a->timing, current_time(a, now, true), a->rate, false, &c);
  if (st != GTEXT_ANIM_OK || !c.in_effect || a->replace != GTEXT_ANIM_REPLACE_ACTIVE) {
    return st;
  }
  a->replace = GTEXT_ANIM_REPLACE_REMOVED;
  const GTEXT_Anim_Time tt = timeline_time(a, now);
  (void)push_event(a, GTEXT_ANIM_EVENT_REMOVE, current_time(a, now, true), tt, tt);
  if (removed) {
    *removed = true;
  }
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_run_pending(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  ENTER(a);
  if ((!a->pending_play && !a->pending_pause) || !active(a, now)) {
    return GTEXT_ANIM_OK;
  }
  const double ready = now.ms;
  if (a->pending_pause) {
    /* the pending pause task */
    a->pending_pause = false;
    if (a->start.resolved && !a->hold.resolved) {
      a->hold = at((ready - a->start.ms) * a->rate);
    }
    apply_pending_rate(a);
    a->start = UNRESOLVED;
    resolve_ready(a);
    update_finished(a, now, false, false);
    return GTEXT_ANIM_OK;
  }
  /* the pending play task */
  a->pending_play = false;
  if (a->hold.resolved) {
    apply_pending_rate(a);
    a->start = a->rate == 0.0 ? at(ready) : at(ready - a->hold.ms / a->rate);
    if (a->rate != 0.0) {
      a->hold = UNRESOLVED;
    }
  } else if (a->start.resolved && a->has_pending_rate) {
    const double match = (ready - a->start.ms) * a->rate;
    apply_pending_rate(a);
    if (a->rate == 0.0) {
      a->hold = at(match);
    }
    a->start = a->rate == 0.0 ? at(ready) : at(ready - match / a->rate);
  }
  resolve_ready(a);
  update_finished(a, now, false, false);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_checkpoint(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  ENTER(a);
  if (a->finish_microtask) {
    a->finish_microtask = false;
    finish_notification_steps(a, now);
  }
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_animation_update_finished(GTEXT_Anim_Animation * a, GTEXT_Anim_Time now) {
  ENTER(a);
  update_finished(a, now, false, false);
  return GTEXT_ANIM_OK;
}

size_t gtext_anim_animation_note_count(const GTEXT_Anim_Animation * a) {
  return a ? a->n_notes : 0;
}

bool gtext_anim_animation_note_at(const GTEXT_Anim_Animation * a, size_t index, GTEXT_Anim_Note * out) {
  if (!a || !out || index >= a->n_notes) {
    return false;
  }
  *out = a->notes[index];
  return true;
}

void gtext_anim_animation_notes_clear(GTEXT_Anim_Animation * a) {
  if (a) {
    a->n_notes = 0;
  }
}

size_t gtext_anim_animation_event_count(const GTEXT_Anim_Animation * a) {
  return a ? a->n_events : 0;
}

GTEXT_Anim_Status gtext_anim_animation_take_events(GTEXT_Anim_Animation * a, uint64_t id, GTEXT_Anim_Events * events) {
  if (!a || !events) {
    return GTEXT_ANIM_INVALID;
  }
  GTEXT_Anim_Status st = gtext_anim_events_reserve(events, a->n_events, 0);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  for (size_t i = 0; i < a->n_events; i++) {
    GTEXT_Anim_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = a->events[i].type;
    ev.animation = id;
    ev.current_time = a->events[i].current;
    ev.timeline_time = a->events[i].timeline;
    ev.scheduled_time = a->events[i].scheduled;
    /* the room was reserved, so an append cannot fail: the list gets all of the events or none */
    (void)gtext_anim_events_append(events, &ev);
  }
  a->n_events = 0;
  return GTEXT_ANIM_OK;
}
