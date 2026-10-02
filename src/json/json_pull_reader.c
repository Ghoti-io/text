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
 * The pull-model JSON reader.
 *
 * The push parser calls the caller; this lets the caller call the parser. YAML
 * has had a pull reader and CSV now has one; this is the third, and it is
 * deliberately the same four calls in the same order, so that a caller who has
 * used one can read the others.
 *
 * It wraps the streaming parser: a private callback copies each event into a
 * queue, and gtext_json_reader_next() takes them off the front.
 *
 * **Every event's bytes are copied.** json_stream.h says it outright - "String
 * and number data are valid only for the duration of the callback invocation" -
 * so a reader that stored the pointer would hand the caller memory the parser
 * has since reused. The copy is what makes the lifetime documented on
 * gtext_json_reader_next() true.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/json/json_stream.h>
#include <ghoti.io/text/macros.h>

#include "json_internal.h"

typedef struct {
  GTEXT_JSON_Event * items;
  size_t head;
  size_t count;
  size_t capacity;
} json_event_queue;

struct GTEXT_JSON_Reader {
  GTEXT_JSON_Stream * stream;
  json_event_queue queue;
  /// The event handed out last, still owned here so its bytes stay valid until
  /// the call after it.
  GTEXT_JSON_Event last_event;
  bool has_last_event;
  bool finished;
  /// The first error the parse reported, repeated on every later call so that a
  /// failure is never mistaken for the end of the document.
  GTEXT_JSON_Status failed;
  const GTEXT_Allocator * alloc;
};

// ===========================================================================
// Events
// ===========================================================================

/* Which events carry bytes, asked once rather than at each of the three places
   that needs the answer. NUMBER and STRING/KEY use different union members, so
   the switch also says which one to read. */
static const char * event_bytes(
    const GTEXT_JSON_Event * event, size_t * len_out) {
  switch (event->type) {
    case GTEXT_JSON_EVT_STRING:
    case GTEXT_JSON_EVT_KEY:
      *len_out = event->as.str.len;
      return event->as.str.s;
    case GTEXT_JSON_EVT_NUMBER:
      *len_out = event->as.number.len;
      return event->as.number.s;
    default:
      *len_out = 0;
      return NULL;
  }
}

static void event_free(
    const GTEXT_Allocator * alloc, GTEXT_JSON_Event * event) {
  size_t len = 0;
  const char * bytes = event_bytes(event, &len);
  if (bytes) {
    // Cast away const: this copy is the queue's, whatever the public type says
    // about the parser's own events.
    gtext_allocator_free(alloc, (void *)(uintptr_t)bytes);
  }
  memset(event, 0, sizeof(*event));
}

/* An empty string is a real value and distinct from no string at all, so it
   still gets a one-byte buffer rather than NULL. */
static bool event_copy(const GTEXT_Allocator * alloc,
    const GTEXT_JSON_Event * src, GTEXT_JSON_Event * dst) {
  memset(dst, 0, sizeof(*dst));
  dst->type = src->type;
  /* Copied here rather than in each arm below: this function has two early
     returns, so a field set after one of them would be carried on some events
     and dropped on others - and a key event, the only one this field means
     anything on, leaves by the *second* return. */
  dst->repeated_key = src->repeated_key;

  size_t len = 0;
  const char * bytes = event_bytes(src, &len);
  if (!bytes && src->type != GTEXT_JSON_EVT_STRING
      && src->type != GTEXT_JSON_EVT_KEY
      && src->type != GTEXT_JSON_EVT_NUMBER) {
    // A structural event, or a bool, or null.
    if (src->type == GTEXT_JSON_EVT_BOOL) {
      dst->as.boolean = src->as.boolean;
    }
    return true;
  }

  char * copy = (char *)gtext_allocator_malloc(alloc, len + 1);
  if (!copy) {
    return false;
  }
  if (len > 0 && bytes) {
    memcpy(copy, bytes, len);
  }
  copy[len] = '\0';

  if (src->type == GTEXT_JSON_EVT_NUMBER) {
    dst->as.number.s = copy;
    dst->as.number.len = len;
  }
  else {
    dst->as.str.s = copy;
    dst->as.str.len = len;
  }
  return true;
}

// ===========================================================================
// The queue
// ===========================================================================

static bool queue_reserve(
    const GTEXT_Allocator * alloc, json_event_queue * queue, size_t needed) {
  if (queue->capacity >= needed) {
    return true;
  }

  size_t new_cap = queue->capacity == 0 ? 8 : queue->capacity * 2;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2) {
      return false;
    }
    new_cap *= 2;
  }

  GTEXT_JSON_Event * items = (GTEXT_JSON_Event *)gtext_allocator_calloc(
      alloc, new_cap, sizeof(*items));
  if (!items) {
    return false;
  }
  for (size_t i = 0; i < queue->count; i++) {
    items[i] = queue->items[(queue->head + i) % queue->capacity];
  }
  gtext_allocator_free(alloc, queue->items);
  queue->items = items;
  queue->capacity = new_cap;
  queue->head = 0;
  return true;
}

static bool queue_push(const GTEXT_Allocator * alloc, json_event_queue * queue,
    const GTEXT_JSON_Event * event) {
  if (!queue_reserve(alloc, queue, queue->count + 1)) {
    return false;
  }
  queue->items[(queue->head + queue->count) % queue->capacity] = *event;
  queue->count++;
  return true;
}

static bool queue_pop(json_event_queue * queue, GTEXT_JSON_Event * event) {
  if (queue->count == 0) {
    return false;
  }
  *event = queue->items[queue->head];
  queue->head = (queue->head + 1) % queue->capacity;
  queue->count--;
  return true;
}

static void queue_clear(
    const GTEXT_Allocator * alloc, json_event_queue * queue) {
  if (!queue->items) {
    return;
  }
  for (size_t i = 0; i < queue->count; i++) {
    event_free(alloc, &queue->items[(queue->head + i) % queue->capacity]);
  }
  gtext_allocator_free(alloc, queue->items);
  queue->items = NULL;
  queue->capacity = 0;
  queue->count = 0;
  queue->head = 0;
}

// ===========================================================================
// The bridge from push to pull
// ===========================================================================

static GTEXT_JSON_Status reader_on_event(
    void * user, const GTEXT_JSON_Event * evt, GTEXT_JSON_Error * err) {
  (void)err;
  GTEXT_JSON_Reader * reader = (GTEXT_JSON_Reader *)user;

  GTEXT_JSON_Event copy;
  if (!event_copy(reader->alloc, evt, &copy)) {
    return GTEXT_JSON_E_OOM;
  }
  if (!queue_push(reader->alloc, &reader->queue, &copy)) {
    event_free(reader->alloc, &copy);
    return GTEXT_JSON_E_OOM;
  }
  return GTEXT_JSON_OK;
}

// ===========================================================================
// Public interface
// ===========================================================================

GTEXT_API GTEXT_JSON_Reader * gtext_json_reader_new(
    const GTEXT_JSON_Parse_Options * opts) {
  const GTEXT_Allocator * alloc = opts ? opts->allocator : NULL;

  GTEXT_JSON_Reader * reader = (GTEXT_JSON_Reader *)gtext_allocator_calloc(
      alloc, 1, sizeof(GTEXT_JSON_Reader));
  if (!reader) {
    return NULL;
  }
  reader->alloc = alloc;
  reader->failed = GTEXT_JSON_OK;

  // The callback needs the reader, so the reader has to exist first.
  reader->stream = gtext_json_stream_new(opts, reader_on_event, reader);
  if (!reader->stream) {
    gtext_allocator_free(alloc, reader);
    return NULL;
  }
  return reader;
}

GTEXT_API GTEXT_JSON_Status gtext_json_reader_feed(GTEXT_JSON_Reader * reader,
    const void * data, size_t len, GTEXT_JSON_Error * err) {
  if (!reader) {
    return GTEXT_JSON_E_INVALID;
  }
  if (reader->failed != GTEXT_JSON_OK) {
    return reader->failed;
  }

  // data == NULL with len == 0 is end of input, as in the YAML and CSV
  // readers. A second one does nothing rather than failing, so a loop that
  // finishes on a short read may finish again.
  if (!data && len == 0) {
    if (reader->finished) {
      return GTEXT_JSON_OK;
    }
    reader->finished = true;
    GTEXT_JSON_Status status = gtext_json_stream_finish(reader->stream, err);
    if (status != GTEXT_JSON_OK) {
      reader->failed = status;
    }
    return status;
  }

  if (reader->finished) {
    return GTEXT_JSON_E_STATE;
  }
  if (!data) {
    return GTEXT_JSON_E_INVALID;
  }

  GTEXT_JSON_Status status =
      gtext_json_stream_feed(reader->stream, (const char *)data, len, err);
  if (status != GTEXT_JSON_OK) {
    reader->failed = status;
  }
  return status;
}

GTEXT_API GTEXT_JSON_Status gtext_json_reader_next(
    GTEXT_JSON_Reader * reader, GTEXT_JSON_Event * out_event) {
  if (!reader || !out_event) {
    return GTEXT_JSON_E_INVALID;
  }

  // Released here rather than when it was handed out, which is what makes the
  // previous event's bytes valid until this call.
  if (reader->has_last_event) {
    event_free(reader->alloc, &reader->last_event);
    reader->has_last_event = false;
  }

  if (!queue_pop(&reader->queue, &reader->last_event)) {
    if (reader->failed != GTEXT_JSON_OK) {
      return reader->failed;
    }
    return reader->finished ? GTEXT_JSON_E_STATE : GTEXT_JSON_E_INCOMPLETE;
  }

  reader->has_last_event = true;
  *out_event = reader->last_event;
  return GTEXT_JSON_OK;
}

GTEXT_API void gtext_json_reader_free(GTEXT_JSON_Reader * reader) {
  if (!reader) {
    return;
  }
  const GTEXT_Allocator * alloc = reader->alloc;
  if (reader->has_last_event) {
    event_free(alloc, &reader->last_event);
  }
  queue_clear(alloc, &reader->queue);
  gtext_json_stream_free(reader->stream);
  gtext_allocator_free(alloc, reader);
}
