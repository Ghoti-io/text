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
 * `@keyframes` as a typed struct (css_keyframes.h): the blocks as written, normalized to one frame per offset in ascending order.
 */

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/css/css_keyframes.h>

#include "css_atrule_internal.h"

typedef struct {
  double offset;
  size_t order;       ///< The block's place among the blocks (document order).
  const GTEXT_CSS_Atrule * block;
} slot;

static int slot_cmp(const void * a, const void * b) {
  const slot * x = a;
  const slot * y = b;
  if (x->offset != y->offset) {
    return x->offset < y->offset ? -1 : 1;
  }
  return x->order < y->order ? -1 : (x->order > y->order);
}

void gtext_css_keyframes_free(GTEXT_CSS_Keyframes * k) {
  if (k) {
    holder * h = k->priv_;
    const GTEXT_Allocator a = h->alloc;
    holder_free(h);
    gtext_allocator_free(&a, k);
  }
}

GTEXT_CSS_Status gtext_css_keyframes_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Keyframes ** out) {
  if (!rule || !out || gtext_css_atrule_kind(rule) != GTEXT_CSS_ATRULE_KEYFRAMES) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  const GTEXT_Allocator * al = &rule->alloc;
  GTEXT_CSS_Keyframes * k = gtext_allocator_calloc(al, 1, sizeof *k);
  if (!k) {
    return GTEXT_CSS_E_OOM;
  }
  holder * h = holder_new(al);
  if (!h) {
    gtext_allocator_free(al, k);
    return GTEXT_CSS_E_OOM;
  }
  k->priv_ = h;
  k->rule = rule;
  bool oom = false;
  GTEXT_CSS_String nm;
  bool quoted = false;
  if (gtext_css_atrule_name_at(rule, 0, &nm, &quoted)) {
    k->name = copy_string(h, &nm, &oom);
    k->name_is_string = quoted;
  }
  k->block_count = rule->nkids;
  size_t total = 0;
  for (size_t i = 0; i < rule->nkids; i++) {
    total += rule->kids[i]->noffsets;
  }
  slot * slots = gtext_allocator_malloc(al, (total ? total : 1) * sizeof *slots);
  if (!slots) {
    gtext_css_keyframes_free(k);
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  for (size_t i = 0; i < rule->nkids; i++) {
    const GTEXT_CSS_Atrule * b = rule->kids[i];
    for (size_t j = 0; j < b->noffsets; j++) {
      slots[n].offset = b->offsets[j];
      slots[n].order = n;
      slots[n].block = b;
      n++;
    }
  }
  qsort(slots, n, sizeof *slots, slot_cmp);
  GTEXT_CSS_Keyframe * frames = css_arena_alloc(&h->arena, (n ? n : 1) * sizeof *frames);
  if (!frames) {
    oom = true;
  }
  size_t nf = 0;
  for (size_t i = 0; frames && i < n && !oom;) {
    size_t j = i;
    size_t props = 0;
    while (j < n && slots[j].offset == slots[i].offset) {
      props += slots[j].block->nds;
      j++;
    }
    GTEXT_CSS_Keyframe_Property * arr = css_arena_alloc(&h->arena, (props ? props : 1) * sizeof *arr);
    if (!arr) {
      oom = true;
      break;
    }
    size_t np = 0;
    for (size_t s = i; s < j; s++) {
      const GTEXT_CSS_Atrule * b = slots[s].block;
      for (size_t d = 0; d < b->nds; d++) {
        const atrule_desc * ds = &b->ds[d];
        GTEXT_CSS_Keyframe_Property p;
        p.property = ds->prop;
        p.name.data = ds->name;
        p.name.length = ds->name_len;
        p.text.data = ds->text;
        p.text.length = ds->text_len;
        p.set = ds->pv.set;
        size_t at = np;
        for (size_t q = 0; q < np; q++) {
          if (arr[q].name.length == p.name.length && memcmp(arr[q].name.data, p.name.data, p.name.length) == 0) {
            at = q;
            break;
          }
        }
        arr[at] = p;
        if (at == np) {
          np++;
        }
      }
    }
    frames[nf].offset = slots[i].offset;
    frames[nf].properties = arr;
    frames[nf].property_count = np;
    nf++;
    i = j;
  }
  gtext_allocator_free(al, slots);
  if (oom) {
    gtext_css_keyframes_free(k);
    return GTEXT_CSS_E_OOM;
  }
  k->frames = frames;
  k->frame_count = nf;
  *out = k;
  return GTEXT_CSS_OK;
}
