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
 * CSSOM's "serialize a selector" and "serialize a group of selectors" (css_selector.h): the text of a compiled selector, canonical, so that a parse of it is
 * the same selector. The recursion follows the nesting of functional pseudo-classes, which the parser bounded by `max_depth`.
 */

#include <string.h>

#include "css_selector_internal.h"

typedef struct {
  const GTEXT_Markup_Sink * sink;
  GTEXT_CSS_Status status;
} W;

static bool put(W * w, const char * s, size_t n) {
  if (w->status != GTEXT_CSS_OK) {
    return false;
  }
  if (n && w->sink->write(w->sink->user, s, n) != GTEXT_MARKUP_OK) {
    w->status = GTEXT_CSS_E_WRITE;
    return false;
  }
  return true;
}

static bool puts_(W * w, const char * s) {
  return put(w, s, strlen(s));
}

static bool put_ident(W * w, const GTEXT_CSS_String * s) {
  if (w->status != GTEXT_CSS_OK) {
    return false;
  }
  if (s->length) {
    const GTEXT_CSS_Status st = gtext_css_serialize_identifier(s->data, s->length, w->sink);
    if (st != GTEXT_CSS_OK) {
      w->status = st;
      return false;
    }
  }
  return true;
}

static bool put_string(W * w, const GTEXT_CSS_String * s) {
  if (w->status != GTEXT_CSS_OK) {
    return false;
  }
  const GTEXT_CSS_Status st = gtext_css_serialize_string(s->data ? s->data : "", s->length, w->sink);
  if (st != GTEXT_CSS_OK) {
    w->status = st;
    return false;
  }
  return true;
}

static void ser_list(W * w, const GTEXT_CSS_Selector_List * l);

static void ser_ns(W * w, const GTEXT_CSS_Simple * s) {
  switch (s->ns_kind) {
  case GTEXT_CSS_NS_ANY: puts_(w, "*|"); break;
  case GTEXT_CSS_NS_NONE: puts_(w, "|"); break;
  case GTEXT_CSS_NS_NAMED:
    put_ident(w, &s->ns_prefix);
    puts_(w, "|");
    break;
  case GTEXT_CSS_NS_UNSPECIFIED: break;
  }
}

static void ser_strings(W * w, const GTEXT_CSS_Simple * s, const char * sep) {
  for (size_t i = 0; i < s->string_count; i++) {
    if (i) {
      puts_(w, sep);
    }
    if (s->string_quoted && s->string_quoted[i]) {
      put_string(w, &s->strings[i]);
    }
    else {
      put_ident(w, &s->strings[i]);
    }
  }
}

static void ser_pseudo(W * w, const GTEXT_CSS_Simple * s) {
  const sel_row * r = css_sel_row_of(s);
  puts_(w, s->kind == GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT ? "::" : ":");
  put(w, r->base, strlen(r->base));
  if (!s->functional) {
    return;
  }
  puts_(w, "(");
  switch ((sel_arg)r->arg) {
  case SELARG_FORGIVING:
  case SELARG_REAL:
  case SELARG_RELATIVE:
  case SELARG_COMPOUND:
  case SELARG_COMPOUND_LIST:
    ser_list(w, s->list);
    break;
  case SELARG_ANB:
  case SELARG_ANB_OF: {
    char buf[64];
    const size_t n = gtext_css_anb_serialize(s->anb_a, s->anb_b, buf, sizeof buf);
    put(w, buf, n < sizeof buf ? n : sizeof buf - 1);
    if (s->has_of) {
      puts_(w, " of ");
      ser_list(w, s->list);
    }
    break;
  }
  case SELARG_LANG: ser_strings(w, s, ", "); break;
  case SELARG_DIR: ser_strings(w, s, ", "); break;
  case SELARG_STATE: ser_strings(w, s, ", "); break;
  case SELARG_IDENTS_COMMA: ser_strings(w, s, ", "); break;
  case SELARG_PART_IDENTS: ser_strings(w, s, " "); break;
  case SELARG_LEVELS:
    for (size_t i = 0; i < s->level_count; i++) {
      if (i) {
        puts_(w, ", ");
      }
      char buf[24];
      size_t n = sizeof buf;
      uint64_t mag = s->levels[i] < 0 ? (uint64_t)(-(s->levels[i] + 1)) + 1u : (uint64_t)s->levels[i];
      do {
        buf[--n] = (char)('0' + (int)(mag % 10u));
        mag /= 10u;
      } while (mag);
      if (s->levels[i] < 0) {
        buf[--n] = '-';
      }
      put(w, buf + n, sizeof buf - n);
    }
    break;
  case SELARG_NAME_OR_STAR:
  case SELARG_ENUM:
    if (s->anb_a == 1) {
      puts_(w, "*");
    }
    else if (r->arg == SELARG_ENUM) {
      put(w, s->strings[0].data, s->strings[0].length);
    }
    else {
      put_ident(w, &s->strings[0]);
    }
    break;
  case SELARG_PT_NAME:
    if (s->anb_a == 1) {
      puts_(w, "*");
    }
    else {
      put_ident(w, &s->strings[0]);
    }
    for (size_t i = 1; i < s->string_count; i++) {
      puts_(w, ".");
      put_ident(w, &s->strings[i]);
    }
    break;
  default: break;
  }
  puts_(w, ")");
}

static void ser_simple(W * w, const GTEXT_CSS_Simple * s) {
  switch (s->kind) {
  case GTEXT_CSS_SIMPLE_TYPE:
    ser_ns(w, s);
    put_ident(w, &s->name);
    break;
  case GTEXT_CSS_SIMPLE_UNIVERSAL:
    ser_ns(w, s);
    puts_(w, "*");
    break;
  case GTEXT_CSS_SIMPLE_ID:
    puts_(w, "#");
    put_ident(w, &s->name);
    break;
  case GTEXT_CSS_SIMPLE_CLASS:
    puts_(w, ".");
    put_ident(w, &s->name);
    break;
  case GTEXT_CSS_SIMPLE_ATTRIBUTE: {
    static const char * const ops[] = {"", "=", "~=", "|=", "^=", "$=", "*="};
    puts_(w, "[");
    ser_ns(w, s);
    put_ident(w, &s->name);
    if (s->op != GTEXT_CSS_ATTR_EXISTS) {
      puts_(w, ops[s->op]);
      put_string(w, &s->value);
      if (s->attr_case == GTEXT_CSS_ATTR_CASE_INSENSITIVE) {
        puts_(w, " i");
      }
      else if (s->attr_case == GTEXT_CSS_ATTR_CASE_SENSITIVE) {
        puts_(w, " s");
      }
    }
    puts_(w, "]");
    break;
  }
  case GTEXT_CSS_SIMPLE_NESTING: puts_(w, "&"); break;
  case GTEXT_CSS_SIMPLE_PSEUDO_CLASS:
  case GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT: ser_pseudo(w, s); break;
  }
}

static void ser_compound(W * w, const GTEXT_CSS_Compound * c) {
  for (size_t i = 0; i < c->simple_count; i++) {
    const GTEXT_CSS_Simple * s = &c->simples[i];
    if (i == 0 && s->kind == GTEXT_CSS_SIMPLE_UNIVERSAL && c->simple_count > 1) {
      // a universal selector that other simple selectors follow is not written, unless it is what keeps the default namespace off them
      if (s->ns_kind == GTEXT_CSS_NS_UNSPECIFIED && s->ns_from_default) {
        puts_(w, "*");
      }
      else if (s->ns_kind == GTEXT_CSS_NS_NAMED || s->ns_kind == GTEXT_CSS_NS_NONE) {
        ser_simple(w, s);
      }
      continue;
    }
    ser_simple(w, s);
  }
}

static void ser_complex(W * w, const GTEXT_CSS_Complex * x) {
  if (x->raw.data) {
    put(w, x->raw.data, x->raw.length);
    return;
  }
  for (size_t i = 0; i < x->compound_count; i++) {
    const GTEXT_CSS_Compound * c = &x->compounds[i];
    const bool lead = i == 0;
    if (lead) {
      if (x->relative && c->combinator != GTEXT_CSS_COMBINATOR_DESCENDANT) {
        switch (c->combinator) {
        case GTEXT_CSS_COMBINATOR_CHILD: puts_(w, "> "); break;
        case GTEXT_CSS_COMBINATOR_NEXT_SIBLING: puts_(w, "+ "); break;
        case GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING: puts_(w, "~ "); break;
        case GTEXT_CSS_COMBINATOR_COLUMN: puts_(w, "|| "); break;
        default: break;
        }
      }
    }
    else {
      switch (c->combinator) {
      case GTEXT_CSS_COMBINATOR_CHILD: puts_(w, " > "); break;
      case GTEXT_CSS_COMBINATOR_NEXT_SIBLING: puts_(w, " + "); break;
      case GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING: puts_(w, " ~ "); break;
      case GTEXT_CSS_COMBINATOR_COLUMN: puts_(w, " || "); break;
      default: puts_(w, " "); break;
      }
    }
    ser_compound(w, c);
  }
}

static void ser_list(W * w, const GTEXT_CSS_Selector_List * l) {
  if (!l) {
    return;
  }
  for (size_t i = 0; i < l->count; i++) {
    if (i) {
      puts_(w, ", ");
    }
    ser_complex(w, &l->complex[i]);
  }
}

GTEXT_CSS_Status gtext_css_selector_serialize(const GTEXT_CSS_Selector_List * list, const GTEXT_Markup_Sink * sink) {
  if (!list || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  W w = {sink, GTEXT_CSS_OK};
  ser_list(&w, list);
  return w.status;
}

GTEXT_CSS_Status gtext_css_selector_serialize_complex(const GTEXT_CSS_Complex * complex, const GTEXT_Markup_Sink * sink) {
  if (!complex || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  W w = {sink, GTEXT_CSS_OK};
  ser_complex(&w, complex);
  return w.status;
}

typedef struct {
  const GTEXT_Allocator * alloc;
  char * data;
  size_t len;
  size_t cap;
  bool oom;
} Buf;

static GTEXT_Markup_Status buf_write(void * user, const char * bytes, size_t len) {
  Buf * b = user;
  if (b->len + len + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + len + 1) {
      cap *= 2;
    }
    char * d = gtext_allocator_realloc(b->alloc, b->data, cap);
    if (!d) {
      b->oom = true;
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = d;
    b->cap = cap;
  }
  memcpy(b->data + b->len, bytes, len);
  b->len += len;
  b->data[b->len] = '\0';
  return GTEXT_MARKUP_OK;
}

GTEXT_CSS_Status gtext_css_selector_serialize_buffer(const GTEXT_CSS_Selector_List * list, const GTEXT_Allocator * allocator, char ** out, size_t * length) {
  if (!list || !out || !length) {
    return GTEXT_CSS_E_INVALID;
  }
  Buf b = {allocator, NULL, 0, 0, false};
  GTEXT_Markup_Sink sink = {buf_write, &b};
  GTEXT_CSS_Status st = gtext_css_selector_serialize(list, &sink);
  if (st == GTEXT_CSS_OK && !b.data) {
    // an empty list: still a string
    b.data = gtext_allocator_malloc(allocator, 1);
    if (!b.data) {
      b.oom = true;
    }
    else {
      b.data[0] = '\0';
    }
  }
  if (st != GTEXT_CSS_OK || b.oom) {
    gtext_allocator_free(allocator, b.data);
    return b.oom ? GTEXT_CSS_E_OOM : st;
  }
  *out = b.data;
  *length = b.len;
  return GTEXT_CSS_OK;
}

void gtext_css_selector_buffer_free(const GTEXT_Allocator * allocator, char * buffer) {
  gtext_allocator_free(allocator, buffer);
}
