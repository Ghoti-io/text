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
 * The list of animation events a frame owes (animation/events.h): the events, the names they carry in a pool of their own, and the stable sort by scheduled event time.
 */

#include <ghoti.io/text/animation/events.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
  GTEXT_Anim_Event_Type type;
  uint64_t animation;
  const void * element;
  size_t pseudo_off, pseudo_len;
  size_t name_off, name_len;
  double elapsed;
  bool resolved;
  double scheduled_ms;
} ev_t;

struct GTEXT_Anim_Events {
  GTEXT_Allocator alloc;
  ev_t * ev;
  size_t n, cap;
  char * pool;
  size_t npool, cappool;
};

GTEXT_Anim_Status gtext_anim_events_new(const GTEXT_Allocator * allocator, GTEXT_Anim_Events ** out) {
  if (!out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  GTEXT_Allocator a = allocator ? *allocator : *gtext_allocator_default();
  GTEXT_Anim_Events * e = gtext_allocator_calloc(&a, 1, sizeof *e);
  if (!e) {
    return GTEXT_ANIM_OOM;
  }
  e->alloc = a;
  *out = e;
  return GTEXT_ANIM_OK;
}

void gtext_anim_events_free(GTEXT_Anim_Events * events) {
  if (!events) {
    return;
  }
  GTEXT_Allocator a = events->alloc;
  gtext_allocator_free(&a, events->ev);
  gtext_allocator_free(&a, events->pool);
  gtext_allocator_free(&a, events);
}

size_t gtext_anim_events_count(const GTEXT_Anim_Events * events) {
  return events ? events->n : 0;
}

bool gtext_anim_events_at(const GTEXT_Anim_Events * events, size_t index, GTEXT_Anim_Event * out) {
  if (!events || !out || index >= events->n) {
    return false;
  }
  const ev_t * e = &events->ev[index];
  out->type = e->type;
  out->animation = e->animation;
  out->element = e->element;
  /* a list whose strings are all empty never made its pool: no pointer is made from NULL */
  out->pseudo_element = (GTEXT_CSS_String){e->pseudo_len ? events->pool + e->pseudo_off : NULL, e->pseudo_len};
  out->name = (GTEXT_CSS_String){e->name_len ? events->pool + e->name_off : NULL, e->name_len};
  out->elapsed_time = e->elapsed;
  out->scheduled_time = (GTEXT_Anim_Time){e->resolved, e->resolved ? e->scheduled_ms : 0.0};
  return true;
}

void gtext_anim_events_clear(GTEXT_Anim_Events * events) {
  if (events) {
    events->n = 0;
    events->npool = 0;
  }
}

GTEXT_Anim_Status gtext_anim_events_reserve(GTEXT_Anim_Events * events, size_t more_events, size_t more_name_bytes) {
  if (!events) {
    return GTEXT_ANIM_INVALID;
  }
  if (more_events > SIZE_MAX / sizeof(ev_t) - events->n || more_name_bytes > SIZE_MAX / 2 - events->npool) {
    return GTEXT_ANIM_OOM;
  }
  if (events->n + more_events > events->cap) {
    const size_t cap = events->n + more_events;
    ev_t * bigger = gtext_allocator_realloc(&events->alloc, events->ev, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_ANIM_OOM;
    }
    events->ev = bigger;
    events->cap = cap;
  }
  if (events->npool + more_name_bytes > events->cappool) {
    const size_t cap = events->npool + more_name_bytes;
    char * bigger = gtext_allocator_realloc(&events->alloc, events->pool, cap ? cap : 1);
    if (!bigger) {
      return GTEXT_ANIM_OOM;
    }
    events->pool = bigger;
    events->cappool = cap;
  }
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_events_append(GTEXT_Anim_Events * events, const GTEXT_Anim_Event * event) {
  if (!events || !event || (!event->name.data && event->name.length) || (!event->pseudo_element.data && event->pseudo_element.length)) {
    return GTEXT_ANIM_INVALID;
  }
  const size_t bytes = event->name.length + event->pseudo_element.length;
  /* room for one more event and for its names, grown geometrically so that a long list is not quadratic */
  GTEXT_Anim_Status st = GTEXT_ANIM_OK;
  if (events->n == events->cap || events->npool + bytes > events->cappool) {
    st = gtext_anim_events_reserve(events, events->n == events->cap ? (events->cap ? events->cap : 8) : 0, events->npool + bytes > events->cappool ? events->cappool + bytes + 64 : 0);
  }
  if (st != GTEXT_ANIM_OK) {
    st = gtext_anim_events_reserve(events, events->n == events->cap ? 1 : 0, events->npool + bytes > events->cappool ? bytes : 0);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
  }
  ev_t * e = &events->ev[events->n++];
  e->type = event->type;
  e->animation = event->animation;
  e->element = event->element;
  e->pseudo_off = events->npool;
  e->pseudo_len = event->pseudo_element.length;
  if (e->pseudo_len) {
    memcpy(events->pool + events->npool, event->pseudo_element.data, e->pseudo_len);
  }
  events->npool += e->pseudo_len;
  e->name_off = events->npool;
  e->name_len = event->name.length;
  if (e->name_len) {
    memcpy(events->pool + events->npool, event->name.data, e->name_len);
  }
  events->npool += e->name_len;
  e->elapsed = event->elapsed_time;
  e->resolved = event->scheduled_time.resolved;
  e->scheduled_ms = event->scheduled_time.resolved ? event->scheduled_time.ms : 0.0;
  return GTEXT_ANIM_OK;
}

/// Whether `a` sorts after `b`: a resolved time sorts after an unresolved one, and a later one after an earlier one.
static bool after(const ev_t * a, const ev_t * b) {
  if (a->resolved != b->resolved) {
    return a->resolved;
  }
  return a->resolved && a->scheduled_ms > b->scheduled_ms;
}

void gtext_anim_events_sort(GTEXT_Anim_Events * l) {
  if (!l || l->n < 2) {
    return;
  }
  const size_t n = l->n;
  ev_t * tmp = gtext_allocator_malloc(&l->alloc, n * sizeof *tmp);
  if (!tmp) {
    /* no memory for a merge: the same stable order by insertion, which needs none */
    for (size_t i = 1; i < n; i++) {
      ev_t x = l->ev[i];
      size_t j = i;
      while (j > 0 && after(&l->ev[j - 1], &x)) {
        l->ev[j] = l->ev[j - 1];
        j--;
      }
      l->ev[j] = x;
    }
    return;
  }
  ev_t * src = l->ev;
  ev_t * dst = tmp;
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      const size_t mid = lo + width < n ? lo + width : n;
      const size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t a = lo, b = mid, k = lo;
      while (a < mid && b < hi) {
        dst[k++] = after(&src[a], &src[b]) ? src[b++] : src[a++];
      }
      while (a < mid) {
        dst[k++] = src[a++];
      }
      while (b < hi) {
        dst[k++] = src[b++];
      }
    }
    ev_t * swap = src;
    src = dst;
    dst = swap;
  }
  if (src != l->ev) {
    memcpy(l->ev, src, n * sizeof *src);
  }
  gtext_allocator_free(&l->alloc, tmp);
}
