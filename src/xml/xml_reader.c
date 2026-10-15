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
 * The pull-model XML reader.
 *
 * It wraps the push parser, as the CSV and YAML readers do: a private callback
 * copies each event into a queue and gtext_xml_reader_next() takes them off the
 * front. The push parser's pointers live only for the duration of its callback,
 * so the copy is the point: one block per event holds the attribute array and
 * every string, and it is released by the call after the one that handed the
 * event out.
 *
 * The queue holds what one feed produced. A document fed in one piece is queued
 * whole; feeding in pieces is how a caller bounds that.
 */

#include <ghoti.io/text/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/xml/xml_stream.h>

#include "xml_internal.h"

typedef struct {
  GTEXT_XML_Event event;
  void * block; /* what the event's pointers point into */
} reader_item;

struct GTEXT_XML_Reader {
  GTEXT_XML_Stream * stream;
  GTEXT_Allocator allocator;
  reader_item * items; /* a ring */
  size_t head;
  size_t count;
  size_t capacity;
  reader_item last;    /* handed out by the latest next(), released by the one after */
  bool has_last;
  bool finished;
  GTEXT_XML_Status failed;
  GTEXT_XML_Error failure;
};

/* ---- Copying an event --------------------------------------------------- */

static size_t text_size(const char * s) {
  return s ? strlen(s) + 1 : 0;
}

static size_t align_up(size_t n) {
  const size_t a = sizeof(void *) > 8 ? sizeof(void *) : 8;
  return (n + a - 1) / a * a;
}

static const char * put_text(char * block, size_t * at, const char * s) {
  if (!s) {
    return NULL;
  }
  char * out = block + *at;
  const size_t n = strlen(s) + 1;
  memcpy(out, s, n);
  *at += n;
  return out;
}

/* The event, and everything it points to, in one allocation. */
static bool item_copy(const GTEXT_Allocator * allocator,
    const GTEXT_XML_Event * src, reader_item * dst) {
  memset(dst, 0, sizeof(*dst));
  size_t total = align_up(src->attribute_count * sizeof(GTEXT_XML_Event_Attribute));
  total += text_size(src->namespace_uri) + text_size(src->prefix)
      + text_size(src->local_name) + (src->data ? src->data_length + 1 : 0)
      + text_size(src->name) + text_size(src->public_id)
      + text_size(src->system_id) + text_size(src->version)
      + text_size(src->encoding);
  for (size_t i = 0; i < src->attribute_count; i++) {
    const GTEXT_XML_Event_Attribute * a = &src->attributes[i];
    total += text_size(a->namespace_uri) + text_size(a->prefix)
        + text_size(a->local_name) + a->value_length + 1;
  }
  char * block = gtext_allocator_malloc(allocator, total ? total : 1);
  if (!block) {
    return false;
  }
  size_t at = align_up(src->attribute_count * sizeof(GTEXT_XML_Event_Attribute));
  GTEXT_XML_Event * e = &dst->event;
  *e = *src;
  e->namespace_uri = put_text(block, &at, src->namespace_uri);
  e->prefix = put_text(block, &at, src->prefix);
  e->local_name = put_text(block, &at, src->local_name);
  e->name = put_text(block, &at, src->name);
  e->public_id = put_text(block, &at, src->public_id);
  e->system_id = put_text(block, &at, src->system_id);
  e->version = put_text(block, &at, src->version);
  e->encoding = put_text(block, &at, src->encoding);
  if (src->data) {
    char * out = block + at;
    if (src->data_length > 0) {
      memcpy(out, src->data, src->data_length);
    }
    out[src->data_length] = '\0';
    at += src->data_length + 1;
    e->data = out;
  }
  e->attributes = NULL;
  if (src->attribute_count > 0) {
    GTEXT_XML_Event_Attribute * out = (GTEXT_XML_Event_Attribute *)(void *)block;
    for (size_t i = 0; i < src->attribute_count; i++) {
      const GTEXT_XML_Event_Attribute * a = &src->attributes[i];
      out[i] = *a;
      out[i].namespace_uri = put_text(block, &at, a->namespace_uri);
      out[i].prefix = put_text(block, &at, a->prefix);
      out[i].local_name = put_text(block, &at, a->local_name);
      char * value = block + at;
      if (a->value_length > 0) {
        memcpy(value, a->value, a->value_length);
      }
      value[a->value_length] = '\0';
      at += a->value_length + 1;
      out[i].value = value;
    }
    e->attributes = out;
  }
  dst->block = block;
  return true;
}

static void item_release(const GTEXT_Allocator * allocator, reader_item * item) {
  gtext_allocator_free(allocator, item->block);
  memset(item, 0, sizeof(*item));
}

/* ---- The queue ---------------------------------------------------------- */

static bool queue_reserve(GTEXT_XML_Reader * r, size_t needed) {
  if (r->capacity >= needed) {
    return true;
  }
  size_t capacity = r->capacity == 0 ? 8 : r->capacity * 2;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2) {
      return false;
    }
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(reader_item)) {
    return false;
  }
  reader_item * grown =
      gtext_allocator_calloc(&r->allocator, capacity, sizeof(reader_item));
  if (!grown) {
    return false;
  }
  for (size_t i = 0; i < r->count; i++) {
    grown[i] = r->items[(r->head + i) % r->capacity];
  }
  gtext_allocator_free(&r->allocator, r->items);
  r->items = grown;
  r->capacity = capacity;
  r->head = 0;
  return true;
}

static GTEXT_XML_Status on_event(const GTEXT_XML_Event * event, void * user) {
  GTEXT_XML_Reader * r = user;
  reader_item copy;
  if (!item_copy(&r->allocator, event, &copy)) {
    return GTEXT_XML_E_OOM;
  }
  if (!queue_reserve(r, r->count + 1)) {
    item_release(&r->allocator, &copy);
    return GTEXT_XML_E_OOM;
  }
  r->items[(r->head + r->count) % r->capacity] = copy;
  r->count++;
  return GTEXT_XML_OK;
}

/* ---- The interface ------------------------------------------------------ */

GTEXT_API GTEXT_XML_Reader * gtext_xml_reader_new(
    const GTEXT_XML_Parse_Options * opts) {
  const GTEXT_Allocator * allocator =
      opts && opts->allocator ? opts->allocator : gtext_allocator_default();
  GTEXT_XML_Reader * r = gtext_allocator_calloc(allocator, 1, sizeof(*r));
  if (!r) {
    return NULL;
  }
  r->allocator = *allocator;
  /* The stream gets the same allocator, through this copy. */
  GTEXT_XML_Parse_Options use = opts ? *opts : gtext_xml_parse_options_default();
  use.allocator = &r->allocator;
  r->stream = gtext_xml_stream_new(&use, on_event, r);
  if (!r->stream) {
    GTEXT_Allocator copy = r->allocator;
    gtext_allocator_free(&copy, r);
    return NULL;
  }
  return r;
}

GTEXT_API GTEXT_XML_Status gtext_xml_reader_feed(GTEXT_XML_Reader * r,
    const void * data, size_t len, GTEXT_XML_Error * err) {
  if (!r) {
    return GTEXT_XML_E_INVALID;
  }
  if (r->failed != GTEXT_XML_OK) {
    if (err) {
      *err = r->failure;
    }
    return r->failed;
  }
  GTEXT_XML_Status status;
  if (!data && len == 0) {
    /* End of input. A second one is accepted and does nothing. */
    if (r->finished) {
      if (err) {
        memset(err, 0, sizeof(*err));
      }
      return GTEXT_XML_OK;
    }
    r->finished = true;
    status = gtext_xml_stream_finish(r->stream, &r->failure);
  }
  else {
    if (r->finished) {
      if (err) {
        memset(err, 0, sizeof(*err));
        err->code = GTEXT_XML_E_STATE;
        err->message = gtext_xml_status_string(GTEXT_XML_E_STATE);
        err->line = 1;
        err->column = 1;
      }
      return GTEXT_XML_E_STATE;
    }
    if (!data) {
      if (err) {
        memset(err, 0, sizeof(*err));
        err->code = GTEXT_XML_E_INVALID;
        err->message = gtext_xml_status_string(GTEXT_XML_E_INVALID);
        err->line = 1;
        err->column = 1;
      }
      return GTEXT_XML_E_INVALID;
    }
    status = gtext_xml_stream_feed(r->stream, data, len, &r->failure);
  }
  if (status != GTEXT_XML_OK) {
    r->failed = status;
  }
  if (err) {
    if (status != GTEXT_XML_OK) {
      *err = r->failure;
    }
    else {
      memset(err, 0, sizeof(*err));
    }
  }
  return status;
}

GTEXT_API GTEXT_XML_Status gtext_xml_reader_next(
    GTEXT_XML_Reader * r, GTEXT_XML_Event * out_event) {
  if (!r || !out_event) {
    return GTEXT_XML_E_INVALID;
  }
  if (xml_stream_resolver_callback_active(r->stream)) {
    xml_stream_note_resolver_reentry(r->stream);
    return GTEXT_XML_E_STATE;
  }
  if (r->has_last) {
    item_release(&r->allocator, &r->last);
    r->has_last = false;
  }
  if (r->count == 0) {
    if (r->failed != GTEXT_XML_OK) {
      return r->failed;
    }
    return r->finished ? GTEXT_XML_E_STATE : GTEXT_XML_E_INCOMPLETE;
  }
  r->last = r->items[r->head];
  r->head = (r->head + 1) % r->capacity;
  r->count--;
  r->has_last = true;
  *out_event = r->last.event;
  return GTEXT_XML_OK;
}

GTEXT_API void gtext_xml_reader_free(GTEXT_XML_Reader * r) {
  if (!r) {
    return;
  }
  if (xml_stream_resolver_callback_active(r->stream)) {
    xml_stream_note_resolver_reentry(r->stream);
    return;
  }
  GTEXT_Allocator allocator = r->allocator;
  if (r->has_last) {
    item_release(&allocator, &r->last);
  }
  for (size_t i = 0; i < r->count; i++) {
    item_release(&allocator, &r->items[(r->head + i) % r->capacity]);
  }
  gtext_allocator_free(&allocator, r->items);
  gtext_xml_stream_free(r->stream);
  gtext_allocator_free(&allocator, r);
}
