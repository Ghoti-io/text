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
 * The animation effects of an element (css_animation.h): the `animation-*` longhands of a computed style, coordinated with `animation-name`.
 */

#include <string.h>

#include <ghoti.io/text/css/css_animation.h>

#include "css_compute_internal.h"

struct GTEXT_CSS_Anim_List {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Anim_Effect * effects;
  size_t count;
  char ** names;               ///< The copy of each effect's name (owned).
  GTEXT_CSS_Easing ** easings; ///< Each effect's easing function (owned).
};

void gtext_css_anim_list_free(GTEXT_CSS_Anim_List * list) {
  if (!list) {
    return;
  }
  const GTEXT_Allocator a = list->alloc;
  for (size_t i = 0; i < list->count; i++) {
    if (list->names) {
      gtext_allocator_free(&a, list->names[i]);
    }
    if (list->easings) {
      gtext_css_easing_free(list->easings[i]);
    }
  }
  gtext_allocator_free(&a, list->names);
  gtext_allocator_free(&a, list->easings);
  gtext_allocator_free(&a, list->effects);
  gtext_allocator_free(&a, list);
}

size_t gtext_css_anim_list_count(const GTEXT_CSS_Anim_List * list) {
  return list ? list->count : 0;
}

bool gtext_css_anim_list_at(const GTEXT_CSS_Anim_List * list, size_t index, GTEXT_CSS_Anim_Effect * out) {
  if (!list || !out || index >= list->count) {
    return false;
  }
  *out = list->effects[index];
  return true;
}

/// The items of a comma list value, or the value alone as the one item.
static void items_of(const GTEXT_CSS_Value * root, const GTEXT_CSS_Value ** items, size_t * n) {
  if (root->kind == GTEXT_CSS_VAL_LIST && root->separator == GTEXT_CSS_SEP_COMMA) {
    *items = root->children;
    *n = root->child_count;
  }
  else {
    *items = root;
    *n = 1;
  }
}

static bool is_keyword(const GTEXT_CSS_Value * v, const char * word) {
  const size_t n = strlen(word);
  return v->kind == GTEXT_CSS_VAL_KEYWORD && v->text.length == n && memcmp(v->text.data, word, n) == 0;
}

/// A <time> in milliseconds.
static bool time_ms(const GTEXT_CSS_Value * v, double * ms) {
  double s = 0;
  if (v->kind == GTEXT_CSS_VAL_DIMENSION && gtext_css_unit_to_canonical(v->unit, v->number, &s) && gtext_css_unit_type(v->unit) == GTEXT_CSS_UNIT_TYPE_TIME) {
    *ms = s * 1000.0;
    return true;
  }
  return false;
}

/// One of the keywords of a table, as its index; -1 for none.
static int keyword_index(const GTEXT_CSS_Value * v, const char * const * words, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (is_keyword(v, words[i])) {
      return (int)i;
    }
  }
  return -1;
}

typedef struct {
  const GTEXT_CSS_Value * items;
  size_t n;
} coord;

static const GTEXT_CSS_Value * pick(const coord * c, size_t i) {
  return &c->items[i % c->n];
}

static bool read_coord(const GTEXT_CSS_Computed_Style * style, const char * name, coord * c) {
  const GTEXT_CSS_Registry * r = style->cc->registry;
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(r, name, strlen(name));
  if (!id) {
    return false;
  }
  const GTEXT_CSS_Value * root = gtext_css_computed_root(style, id);
  if (!root) {
    return false;
  }
  items_of(root, &c->items, &c->n);
  return c->n > 0;
}

GTEXT_CSS_Status gtext_css_anim_list_from_style(const GTEXT_CSS_Computed_Style * style, const GTEXT_Allocator * allocator, GTEXT_CSS_Anim_List ** out) {
  if (!style || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * al = allocator ? allocator : &style->alloc;
  coord name, duration, timing, delay_start, delay_end, count, direction, play_state, fill, composition, timeline;
  if (!read_coord(style, "animation-name", &name) || !read_coord(style, "animation-duration", &duration) || !read_coord(style, "animation-timing-function", &timing) ||
      !read_coord(style, "animation-delay-start", &delay_start) || !read_coord(style, "animation-delay-end", &delay_end) ||
      !read_coord(style, "animation-iteration-count", &count) || !read_coord(style, "animation-direction", &direction) ||
      !read_coord(style, "animation-play-state", &play_state) || !read_coord(style, "animation-fill-mode", &fill) ||
      !read_coord(style, "animation-composition", &composition) || !read_coord(style, "animation-timeline", &timeline)) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  static const char * const DIRECTIONS[] = {"normal", "reverse", "alternate", "alternate-reverse"};
  static const char * const FILLS[] = {"none", "forwards", "backwards", "both"};
  static const char * const COMPOSITES[] = {"replace", "add", "accumulate"};
  GTEXT_CSS_Anim_List * list = gtext_allocator_calloc(al, 1, sizeof *list);
  if (!list) {
    return GTEXT_CSS_E_OOM;
  }
  list->alloc = *al;
  size_t effects = 0;
  for (size_t i = 0; i < name.n; i++) {
    if (!is_keyword(&name.items[i], "none")) {
      effects++;
    }
  }
  if (effects) {
    list->effects = gtext_allocator_calloc(al, effects, sizeof *list->effects);
    list->names = gtext_allocator_calloc(al, effects, sizeof *list->names);
    list->easings = gtext_allocator_calloc(al, effects, sizeof *list->easings);
    if (!list->effects || !list->names || !list->easings) {
      gtext_css_anim_list_free(list);
      return GTEXT_CSS_E_OOM;
    }
  }
  /* `count` is raised as each slot is filled, so that a failure frees exactly what was made */
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  for (size_t i = 0; i < name.n && st == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Value * nv = &name.items[i];
    if (is_keyword(nv, "none")) {
      continue;
    }
    GTEXT_CSS_Anim_Effect * e = &list->effects[list->count];
    e->index = i;
    char * copy = gtext_allocator_malloc(al, nv->text.length + 1);
    if (!copy) {
      st = GTEXT_CSS_E_OOM;
      break;
    }
    memcpy(copy, nv->text.data, nv->text.length);
    copy[nv->text.length] = '\0';
    list->names[list->count] = copy;
    list->count++;
    e->name.data = copy;
    e->name.length = nv->text.length;
    e->name_is_string = nv->kind == GTEXT_CSS_VAL_STRING;
    const GTEXT_CSS_Value * v = pick(&duration, i);
    if (is_keyword(v, "auto")) {
      e->duration_auto = true;
    }
    else if (!time_ms(v, &e->duration_ms)) {
      st = GTEXT_CSS_E_UNSUPPORTED;
      break;
    }
    if (!time_ms(pick(&delay_start, i), &e->delay_start_ms) || !time_ms(pick(&delay_end, i), &e->delay_end_ms)) {
      st = GTEXT_CSS_E_UNSUPPORTED;
      break;
    }
    v = pick(&count, i);
    if (is_keyword(v, "infinite")) {
      e->infinite = true;
    }
    else if (v->kind == GTEXT_CSS_VAL_NUMBER || v->kind == GTEXT_CSS_VAL_INTEGER || v->kind == GTEXT_CSS_VAL_ZERO) {
      e->iteration_count = v->number;
    }
    else {
      st = GTEXT_CSS_E_UNSUPPORTED;
      break;
    }
    const int d = keyword_index(pick(&direction, i), DIRECTIONS, 4);
    const int f = keyword_index(pick(&fill, i), FILLS, 4);
    const int c = keyword_index(pick(&composition, i), COMPOSITES, 3);
    if (d < 0 || f < 0 || c < 0) {
      st = GTEXT_CSS_E_UNSUPPORTED;
      break;
    }
    e->direction = (GTEXT_CSS_Anim_Direction)d;
    e->fill = (GTEXT_CSS_Anim_Fill)f;
    e->composite = (GTEXT_CSS_Anim_Composite)c;
    e->paused = is_keyword(pick(&play_state, i), "paused");
    e->timeline_auto = is_keyword(pick(&timeline, i), "auto");
    bool valid = false;
    GTEXT_CSS_Easing * ease = NULL;
    st = gtext_css_easing_from_value(pick(&timing, i), al, &valid, &ease);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    if (!valid) {
      st = GTEXT_CSS_E_UNSUPPORTED;
      break;
    }
    list->easings[list->count - 1] = ease;
    e->easing = ease;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_anim_list_free(list);
    return st;
  }
  *out = list;
  return GTEXT_CSS_OK;
}
