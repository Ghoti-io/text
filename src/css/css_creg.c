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
 * Custom property registrations (css_creg.h).
 */

#include "css_creg.h"

#include <string.h>

#include "css_internal.h"

void css_creg_free(css_creg * c) {
  if (!c) {
    return;
  }
  GTEXT_Allocator a = c->alloc;
  if (c->syntax && c->compiled) {
    for (size_t i = 0; i < c->syntax->ncomps; i++) {
      gtext_css_compiled_free(c->compiled[i]);
    }
  }
  gtext_allocator_free(&a, c->compiled);
  css_syntax_free(c->syntax);
  gtext_allocator_free(&a, c->name);
  gtext_allocator_free(&a, c->syntax_text);
  gtext_allocator_free(&a, c->initial);
  gtext_allocator_free(&a, c);
}

void css_compiled_syntax_free(css_compiled_syntax * s) {
  if (!s) {
    return;
  }
  GTEXT_Allocator a = s->alloc;
  if (s->syntax && s->compiled) {
    for (size_t i = 0; i < s->syntax->ncomps; i++) {
      gtext_css_compiled_free(s->compiled[i]);
    }
  }
  gtext_allocator_free(&a, s->compiled);
  css_syntax_free(s->syntax);
  gtext_allocator_free(&a, s->text);
  gtext_allocator_free(&a, s);
}

bool css_creg_name_ok(const char * name, size_t length) {
  return name && length >= 3 && name[0] == '-' && name[1] == '-';
}

/* ---- the store ------------------------------------------------------------------------------------- */

void css_creg_store_init(css_creg_store * s, const GTEXT_Allocator * alloc) {
  memset(s, 0, sizeof *s);
  s->alloc = *alloc;
}

void css_creg_store_free(css_creg_store * s) {
  for (size_t i = 0; i < s->count; i++) {
    css_creg_free(s->items[i]);
  }
  gtext_allocator_free(&s->alloc, s->items);
  gtext_allocator_free(&s->alloc, s->slots);
  memset(s, 0, sizeof *s);
}

static uint64_t name_hash(const char * s, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
  }
  return h ^ (h >> 29);
}

const css_creg * css_creg_store_find(const css_creg_store * s, const char * name, size_t length, size_t * work) {
  if (!s->slot_cap) {
    return NULL;
  }
  size_t i = (size_t)(name_hash(name, length) & (s->slot_cap - 1));
  if (work) {
    *work += length;
  }
  for (;;) {
    if (work) {
      (*work)++;
    }
    const size_t slot = s->slots[i];
    if (!slot) {
      return NULL;
    }
    const css_creg * c = s->items[slot - 1];
    if (c->name_len == length && memcmp(c->name, name, length) == 0) {
      return c;
    }
    i = (i + 1) & (s->slot_cap - 1);
  }
}

static GTEXT_CSS_Status slots_grow(css_creg_store * s) {
  const size_t cap = s->slot_cap ? s->slot_cap * 2 : 32;
  size_t * g = gtext_allocator_calloc(&s->alloc, cap, sizeof *g);
  if (!g) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t k = 0; k < s->count; k++) {
    const css_creg * c = s->items[k];
    size_t i = (size_t)(name_hash(c->name, c->name_len) & (cap - 1));
    while (g[i]) {
      i = (i + 1) & (cap - 1);
    }
    g[i] = k + 1;
  }
  gtext_allocator_free(&s->alloc, s->slots);
  s->slots = g;
  s->slot_cap = cap;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_creg_store_add(css_creg_store * s, css_creg * c) {
  if (css_creg_store_find(s, c->name, c->name_len, NULL)) {
    return GTEXT_CSS_E_EXISTS;
  }
  if ((s->count + 1) * 2 > s->slot_cap) {
    const GTEXT_CSS_Status st = slots_grow(s);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  if (s->count == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 16;
    css_creg ** g = gtext_allocator_realloc(&s->alloc, s->items, cap * sizeof *g);
    if (!g) {
      return GTEXT_CSS_E_OOM;
    }
    s->items = g;
    s->cap = cap;
  }
  s->items[s->count++] = c;
  size_t i = (size_t)(name_hash(c->name, c->name_len) & (s->slot_cap - 1));
  while (s->slots[i]) {
    i = (i + 1) & (s->slot_cap - 1);
  }
  s->slots[i] = s->count;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_creg_store_put(css_creg_store * s, css_creg * c, bool * replaced) {
  *replaced = false;
  if (s->slot_cap) {
    size_t i = (size_t)(name_hash(c->name, c->name_len) & (s->slot_cap - 1));
    while (s->slots[i]) {
      css_creg * old = s->items[s->slots[i] - 1];
      if (old->name_len == c->name_len && memcmp(old->name, c->name, c->name_len) == 0) {
        s->items[s->slots[i] - 1] = c;
        css_creg_free(old);
        *replaced = true;
        return GTEXT_CSS_OK;
      }
      i = (i + 1) & (s->slot_cap - 1);
    }
  }
  return css_creg_store_add(s, c);
}

/* ---- computational independence ------------------------------------------------------------------------ */

static bool relative_unit(const GTEXT_CSS_String * unit) {
  static const char * const names[] = {"em", "ex", "cap", "ch", "ic", "lh", "rem", "rex", "rcap", "rch", "ric", "rlh",
                                       "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (css_ascii_ieq(unit, names[i])) {
      return true;
    }
  }
  return false;
}

typedef struct {
  const GTEXT_CSS_Component_Value * v;
  size_t n, i;
} cv_frame;

typedef struct {
  const GTEXT_CSS_Value * v;
  size_t n, i;
} val_frame;

GTEXT_CSS_Status css_creg_independent(const GTEXT_Allocator * alloc, const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Value_Set * typed,
    bool * independent) {
  GTEXT_Allocator a = *alloc;
  *independent = true;
  /* the tokens: a unit of the element's font or container, `currentcolor` (`light-dark()` is not one: the pinned property-registration page registers `light-dark(none, none)`
     and every browser accepts `light-dark(red, blue)` as an initial value, which is computed in the environment's color scheme) */
  if (count) {
    size_t cap = 16, len = 0;
    cv_frame * st = gtext_allocator_malloc(&a, cap * sizeof *st);
    if (!st) {
      return GTEXT_CSS_E_OOM;
    }
    st[len++] = (cv_frame){values, count, 0};
    while (len && *independent) {
      cv_frame * f = &st[len - 1];
      if (f->i >= f->n) {
        len--;
        continue;
      }
      const GTEXT_CSS_Component_Value * v = &f->v[f->i++];
      if (v->kind == GTEXT_CSS_VALUE_TOKEN) {
        if ((v->type == GTEXT_CSS_TOKEN_DIMENSION && relative_unit(&v->unit)) || (v->type == GTEXT_CSS_TOKEN_IDENT && css_ascii_ieq(&v->value, "currentcolor"))) {
          *independent = false;
        }
        continue;
      }
      if (v->child_count) {
        if (len == cap) {
          cv_frame * g = gtext_allocator_realloc(&a, st, cap * 2 * sizeof *g);
          if (!g) {
            gtext_allocator_free(&a, st);
            return GTEXT_CSS_E_OOM;
          }
          st = g;
          cap *= 2;
        }
        st[len++] = (cv_frame){v->children, v->child_count, 0};
      }
    }
    gtext_allocator_free(&a, st);
  }
  /* the typed value: a color that is the user's (a system color) or the element's */
  const GTEXT_CSS_Value * root = typed ? gtext_css_value_set_root(typed) : NULL;
  if (root && *independent) {
    size_t cap = 16, len = 0;
    val_frame * st = gtext_allocator_malloc(&a, cap * sizeof *st);
    if (!st) {
      return GTEXT_CSS_E_OOM;
    }
    st[len++] = (val_frame){root, 1, 0};
    while (len && *independent) {
      val_frame * f = &st[len - 1];
      if (f->i >= f->n) {
        len--;
        continue;
      }
      const GTEXT_CSS_Value * v = &f->v[f->i++];
      if (v->kind == GTEXT_CSS_VAL_COLOR && v->color && v->color->kind == GTEXT_CSS_COLOR_KEYWORD &&
          (v->color->keyword == GTEXT_CSS_COLOR_KW_CURRENTCOLOR || v->color->keyword == GTEXT_CSS_COLOR_KW_SYSTEM ||
           v->color->keyword == GTEXT_CSS_COLOR_KW_DEPRECATED)) {
        *independent = false;
        break;
      }
      if (v->child_count) {
        if (len == cap) {
          val_frame * g = gtext_allocator_realloc(&a, st, cap * 2 * sizeof *g);
          if (!g) {
            gtext_allocator_free(&a, st);
            return GTEXT_CSS_E_OOM;
          }
          st = g;
          cap *= 2;
        }
        st[len++] = (val_frame){v->children, v->child_count, 0};
      }
    }
    gtext_allocator_free(&a, st);
  }
  return GTEXT_CSS_OK;
}
