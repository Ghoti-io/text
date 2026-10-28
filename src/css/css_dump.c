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
 * The canonical dump of a computed style and its SHA-256 (css_computed.h).
 */

#include "css_compute_internal.h"

#include <ghoti.io/security/sha256.h>
#include <ghoti.io/text/css/css_registry.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
  const char * name;
  size_t length;
  size_t index;
} named;

static int by_name(const void * x, const void * y) {
  const named * a = x;
  const named * b = y;
  const size_t n = a->length < b->length ? a->length : b->length;
  const int c = n ? memcmp(a->name, b->name, n) : 0;
  if (c != 0) {
    return c;
  }
  return a->length < b->length ? -1 : a->length > b->length ? 1 : 0;
}

static GTEXT_CSS_Status put(const GTEXT_Markup_Sink * sink, const char * data, size_t n) {
  if (n == 0) {
    return GTEXT_CSS_OK;
  }
  return sink->write(sink->user, data, n) == GTEXT_MARKUP_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_WRITE;
}

/* The dump is one line for each property, and a custom property keeps the white space it was written with, which can hold a line break (a white space token
   is any run of them). A line break in a value is the white space it is, so the value is written with a space for each: the same tokens. */
typedef struct {
  const GTEXT_Markup_Sink * out;
} flat_sink;

static GTEXT_Markup_Status flat_write(void * user, const char * data, size_t n) {
  const flat_sink * f = user;
  char buf[256];
  while (n) {
    const size_t m = n < sizeof buf ? n : sizeof buf;
    for (size_t i = 0; i < m; i++) {
      buf[i] = data[i] == '\n' || data[i] == '\r' || data[i] == '\f' ? ' ' : data[i];
    }
    const GTEXT_Markup_Status st = f->out->write(f->out->user, buf, m);
    if (st != GTEXT_MARKUP_OK) {
      return st;
    }
    data += m;
    n -= m;
  }
  return GTEXT_MARKUP_OK;
}

static GTEXT_CSS_Status dump_to(const GTEXT_CSS_Computed_Style * style, const GTEXT_Markup_Sink * sink) {
  GTEXT_Allocator a = style->alloc;
  size_t count = 0;
  for (size_t i = 0; i < style->n; i++) {
    if (css_style_visible(style, i)) {
      count++;
    }
  }
  named * props = gtext_allocator_malloc(&a, (count ? count : 1) * sizeof *props);
  if (!props) {
    return GTEXT_CSS_E_OOM;
  }
  size_t k = 0;
  for (size_t i = 0; i < style->n; i++) {
    if (css_style_visible(style, i)) {
      props[k].name = style->cc->meta[i].name;
      props[k].length = style->cc->meta[i].name_length;
      props[k].index = i;
      k++;
    }
  }
  qsort(props, count, sizeof *props, by_name);
  const char * pin = gtext_css_registry_pin_commit();
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  {
    static const char first[] = "css-computed 1\nregistry ";
    /* the version in the first line is the header's: a change of one is a change of the other */
    _Static_assert(GTEXT_CSS_COMPUTED_DUMP_VERSION == 1, "the first line of the dump names the version");
    st = put(sink, first, sizeof first - 1);
    if (st == GTEXT_CSS_OK) {
      st = put(sink, pin ? pin : "", pin ? strlen(pin) : 0);
    }
    if (st == GTEXT_CSS_OK) {
      st = put(sink, "\n", 1);
    }
  }
  const size_t customs = gtext_css_computed_custom_count(style);
  size_t pi = 0, ci = 0;
  GTEXT_CSS_String cname = {NULL, 0};
  const GTEXT_CSS_Custom_Value * cvalue = NULL;
  bool have = false; /* cname and cvalue are the ci'th custom property */
  while (st == GTEXT_CSS_OK && (pi < count || ci < customs)) {
    if (ci < customs && !have) {
      have = gtext_css_computed_custom_at(style, ci, &cname, &cvalue);
      if (!have) {
        ci = customs;
      }
    }
    bool custom = false;
    if (have) {
      named x = {cname.data, cname.length, 0};
      custom = pi >= count || by_name(&x, &props[pi]) < 0;
    }
    if (custom) {
      ci++;
      have = false;
      st = put(sink, cname.data, cname.length);
      if (st == GTEXT_CSS_OK) {
        st = put(sink, ": ", 2);
      }
      if (st == GTEXT_CSS_OK) {
        flat_sink f = {sink};
        GTEXT_Markup_Sink flat = *sink;
        flat.user = &f;
        flat.write = flat_write;
        st = gtext_css_custom_value_write(cvalue, &flat);
      }
    }
    else {
      const named * p = &props[pi++];
      st = put(sink, p->name, p->length);
      if (st == GTEXT_CSS_OK) {
        st = put(sink, ": ", 2);
      }
      const GTEXT_CSS_Value * root = gtext_css_value_set_root(style->vals[p->index]);
      if (st == GTEXT_CSS_OK && root) {
        st = gtext_css_serialize_value(root, NULL, sink);
      }
    }
    if (st == GTEXT_CSS_OK) {
      st = put(sink, "\n", 1);
    }
  }
  gtext_allocator_free(&a, props);
  return st;
}

GTEXT_CSS_Status gtext_css_computed_dump(const GTEXT_CSS_Computed_Style * style, const GTEXT_Markup_Sink * sink) {
  if (!style || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  return dump_to(style, sink);
}

typedef struct {
  GSEC_Sha256 ctx;
  bool failed;
} hasher;

static GTEXT_Markup_Status hash_write(void * user, const char * data, size_t n) {
  hasher * h = user;
  if (gsec_sha256_update(&h->ctx, data, n) != GSEC_OK) {
    h->failed = true;
    return GTEXT_MARKUP_E_OOM;
  }
  return GTEXT_MARKUP_OK;
}

GTEXT_CSS_Status gtext_css_computed_hash(const GTEXT_CSS_Computed_Style * style, unsigned char hash[GTEXT_CSS_COMPUTED_HASH_LENGTH]) {
  if (!style || !hash) {
    return GTEXT_CSS_E_INVALID;
  }
  hasher h;
  h.failed = false;
  if (gsec_sha256_init(&h.ctx) != GSEC_OK) {
    return GTEXT_CSS_E_INTERNAL;
  }
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &h;
  sink.write = hash_write;
  const GTEXT_CSS_Status st = dump_to(style, &sink);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_WRITE ? GTEXT_CSS_E_INTERNAL : st;
  }
  if (gsec_sha256_final(&h.ctx, hash) != GSEC_OK) {
    return GTEXT_CSS_E_INTERNAL;
  }
  return GTEXT_CSS_OK;
}
