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
 * Specified-value serialization: a typed value written as CSSOM writes it.
 *
 * The walk has an explicit stack (a list in a function in a list is a frame
 * each), numbers are laid out by the CSSOM number idiom the syntax serializer
 * uses (css_cssom_number()), identifiers and strings go through the public
 * serializers of css_serialize.h, and the bytes are counted against the output
 * limit on their way to the caller's sink.
 *
 * Reference: CSSOM, "serialize a CSS component value" (numbers, dimensions,
 * percentages, strings, identifiers, urls) and the separators of "serialize a
 * list": a space, `, ` and ` / `.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "css_internal.h"
#include "css_value_internal.h"

typedef struct {
  const GTEXT_Markup_Sink * sink;
  size_t max;
  size_t written;
  GTEXT_CSS_Status status;
  GTEXT_Allocator alloc;
  GTEXT_Markup_Sink inner; ///< The sink handed to the identifier and string serializers: it counts.
} vwriter;

static GTEXT_Markup_Status counting_write(void * user, const char * bytes, size_t len) {
  vwriter * w = user;
  if (w->status != GTEXT_CSS_OK) {
    return GTEXT_MARKUP_E_WRITE;
  }
  if (len > w->max - w->written) {
    w->status = GTEXT_CSS_E_LIMIT_OUTPUT;
    return GTEXT_MARKUP_E_WRITE;
  }
  if (len && w->sink->write(w->sink->user, bytes, len) != GTEXT_MARKUP_OK) {
    w->status = GTEXT_CSS_E_WRITE;
    return GTEXT_MARKUP_E_WRITE;
  }
  w->written += len;
  return GTEXT_MARKUP_OK;
}

static void put(vwriter * w, const char * s, size_t n) {
  (void)counting_write(w, s, n);
}

static void puts_(vwriter * w, const char * s) {
  put(w, s, strlen(s));
}

static void put_ident(vwriter * w, const GTEXT_CSS_String * s) {
  if (w->status == GTEXT_CSS_OK && s->length) {
    (void)gtext_css_serialize_identifier(s->data, s->length, &w->inner);
  }
}

static void put_string(vwriter * w, const GTEXT_CSS_String * s) {
  if (w->status == GTEXT_CSS_OK) {
    (void)gtext_css_serialize_string(s->data ? s->data : "", s->length, &w->inner);
  }
}

static void put_number(vwriter * w, double v, bool integer) {
  char b[330];
  put(w, b, css_cssom_number(v, integer, b));
}

static void put_hex(vwriter * w, uint32_t v) {
  static const char hex[] = "0123456789ABCDEF";
  char d[8], b[8];
  size_t k = 0, n = 0;
  do {
    d[k++] = hex[v & 0xF];
    v >>= 4;
  } while (v);
  while (k) {
    b[n++] = d[--k];
  }
  put(w, b, n);
}

typedef struct {
  const GTEXT_CSS_Value * v;
  size_t i;
} vframe;

/// Whether a child of a list is the literal comma token (which attaches to the item before it).
static bool is_comma_token(const GTEXT_CSS_Value * c) {
  return c->kind == GTEXT_CSS_VAL_TOKEN && c->text.length == 1 && c->text.data[0] == ',';
}

/// Whether the first thing written for `c` is the literal comma (so that it attaches to what precedes).
static bool begins_with_comma(const GTEXT_CSS_Value * c) {
  while (c->kind == GTEXT_CSS_VAL_LIST && c->separator == GTEXT_CSS_SEP_SPACE && c->child_count > 0) {
    c = &c->children[0];
  }
  return is_comma_token(c);
}

/// What is written for `v` before its children; false if it has none to write after.
static bool open_value(vwriter * w, const GTEXT_CSS_Value * v) {
  switch (v->kind) {
  case GTEXT_CSS_VAL_KEYWORD: case GTEXT_CSS_VAL_CUSTOM_IDENT: case GTEXT_CSS_VAL_DASHED_IDENT:
    put_ident(w, &v->text);
    return false;
  case GTEXT_CSS_VAL_STRING:
    put_string(w, &v->text);
    return false;
  case GTEXT_CSS_VAL_URL:
    puts_(w, "url(");
    put_string(w, &v->text);
    return true;
  case GTEXT_CSS_VAL_NUMBER:
    put_number(w, v->number, false);
    return false;
  case GTEXT_CSS_VAL_INTEGER:
    put_number(w, v->number, true);
    return false;
  case GTEXT_CSS_VAL_PERCENTAGE:
    put_number(w, v->number, false);
    puts_(w, "%");
    return false;
  case GTEXT_CSS_VAL_DIMENSION: {
    put_number(w, v->number, false);
    const char * name = gtext_css_unit_name(v->unit);
    if (name) {
      puts_(w, name);
    }
    else {
      put_ident(w, &v->text);
    }
    return false;
  }
  case GTEXT_CSS_VAL_RATIO:
    put_number(w, v->number, false);
    if (v->flags & GTEXT_CSS_VFLAG_RATIO_DENOMINATOR) {
      puts_(w, " / ");
      put_number(w, v->number2, false);
    }
    return false;
  case GTEXT_CSS_VAL_ZERO:
    puts_(w, "0");
    return false;
  case GTEXT_CSS_VAL_UNICODE_RANGE:
    puts_(w, "U+");
    put_hex(w, v->range_start);
    if (v->range_end != v->range_start) {
      puts_(w, "-");
      put_hex(w, v->range_end);
    }
    return false;
  case GTEXT_CSS_VAL_PENDING: case GTEXT_CSS_VAL_UNPARSED: case GTEXT_CSS_VAL_TOKEN:
    put(w, v->text.data, v->text.length);
    return false;
  case GTEXT_CSS_VAL_FUNCTION:
    put_ident(w, &v->text);
    puts_(w, "(");
    return true;
  case GTEXT_CSS_VAL_BLOCK:
    put(w, v->text.data, v->text.length);
    return true;
  case GTEXT_CSS_VAL_LIST:
    return true;
  default:
    w->status = GTEXT_CSS_E_INVALID;
    return false;
  }
}

static void close_value(vwriter * w, const GTEXT_CSS_Value * v) {
  switch (v->kind) {
  case GTEXT_CSS_VAL_URL: case GTEXT_CSS_VAL_FUNCTION:
    puts_(w, ")");
    break;
  case GTEXT_CSS_VAL_BLOCK:
    puts_(w, v->text.length && v->text.data[0] == '{' ? "}" : v->text.length && v->text.data[0] == '[' ? "]" : ")");
    break;
  default:
    break;
  }
}

static void put_separator(vwriter * w, const GTEXT_CSS_Value * parent, const GTEXT_CSS_Value * child) {
  if (parent->kind == GTEXT_CSS_VAL_LIST) {
    switch (parent->separator) {
    case GTEXT_CSS_SEP_COMMA: puts_(w, ", "); return;
    case GTEXT_CSS_SEP_SLASH: puts_(w, " / "); return;
    default: break;
    }
    if (begins_with_comma(child)) {
      return;
    }
  }
  puts_(w, " ");
}

GTEXT_CSS_Status gtext_css_serialize_value(const GTEXT_CSS_Value * value, const GTEXT_CSS_Serialize_Options * options,
    const GTEXT_Markup_Sink * sink) {
  if (!value || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  vwriter w;
  memset(&w, 0, sizeof w);
  w.sink = sink;
  w.max = (options && options->max_output_bytes) ? options->max_output_bytes : ((size_t)256 * 1024 * 1024);
  w.alloc = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
  w.inner.write = counting_write;
  w.inner.user = &w;
  vframe * stack = NULL;
  size_t len = 0, cap = 0;
  GTEXT_CSS_Status oom = GTEXT_CSS_OK;
  if (open_value(&w, value) && value->child_count) {
    cap = 16;
    stack = gtext_allocator_malloc(&w.alloc, cap * sizeof *stack);
    if (!stack) {
      oom = GTEXT_CSS_E_OOM;
    }
    else {
      stack[len++] = (vframe){value, 0};
    }
  }
  else if (value->child_count == 0) {
    close_value(&w, value);
  }
  while (len > 0 && w.status == GTEXT_CSS_OK && oom == GTEXT_CSS_OK) {
    vframe * f = &stack[len - 1];
    if (f->i >= f->v->child_count) {
      close_value(&w, f->v);
      len--;
      continue;
    }
    const GTEXT_CSS_Value * c = &f->v->children[f->i];
    if (f->i > 0 || f->v->kind == GTEXT_CSS_VAL_URL) {
      put_separator(&w, f->v, c);
    }
    f->i++;
    if (open_value(&w, c)) {
      if (c->child_count == 0) {
        close_value(&w, c);
        continue;
      }
      if (len == cap) {
        if (cap > SIZE_MAX / 2 / sizeof *stack) {
          oom = GTEXT_CSS_E_OOM;
          break;
        }
        vframe * grown = gtext_allocator_realloc(&w.alloc, stack, cap * 2 * sizeof *stack);
        if (!grown) {
          oom = GTEXT_CSS_E_OOM;
          break;
        }
        stack = grown;
        cap *= 2;
      }
      stack[len++] = (vframe){c, 0};
    }
  }
  gtext_allocator_free(&w.alloc, stack);
  if (oom != GTEXT_CSS_OK) {
    return oom;
  }
  return w.status;
}

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
} vmembuf;

static GTEXT_Markup_Status vmembuf_write(void * user, const char * bytes, size_t n) {
  vmembuf * b = user;
  if (n > SIZE_MAX - b->len - 1) {
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + n + 1) {
      if (cap > SIZE_MAX / 2) {
        return GTEXT_MARKUP_E_OOM;
      }
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!grown) {
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, bytes, n);
  b->len += n;
  return GTEXT_MARKUP_OK;
}

GTEXT_CSS_Status gtext_css_serialize_value_buffer(const GTEXT_CSS_Value * value,
    const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length) {
  if (!out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  vmembuf b;
  memset(&b, 0, sizeof b);
  b.alloc = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
  GTEXT_Markup_Sink sink = {vmembuf_write, &b};
  GTEXT_CSS_Status st = gtext_css_serialize_value(value, options, &sink);
  if (st == GTEXT_CSS_E_WRITE) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK && !b.data) {
    b.data = gtext_allocator_malloc(&b.alloc, 1);
    if (!b.data) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&b.alloc, b.data);
    return st;
  }
  b.data[b.len] = '\0';
  *out = b.data;
  *out_length = b.len;
  return GTEXT_CSS_OK;
}
