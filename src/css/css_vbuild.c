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
 * The builder of the value sets that computing makes: a set of its own, values in its arena, and a list of the sets its values point into (so
 * that a value that is not changed is shared and not copied).
 */

#include <string.h>

#include "css_compute_internal.h"

bool css_vb_begin(css_vb * vb, const GTEXT_Allocator * alloc, size_t max_nodes) {
  memset(vb, 0, sizeof *vb);
  vb->alloc = *alloc;
  vb->max_nodes = max_nodes ? max_nodes : (size_t)1 << 20;
  vb->set = css_vset_new(alloc);
  if (!vb->set) {
    vb->status = GTEXT_CSS_E_OOM;
    return false;
  }
  return true;
}

void css_vb_fail(css_vb * vb, GTEXT_CSS_Status status) {
  if (vb->status == GTEXT_CSS_OK) {
    vb->status = status;
  }
}

GTEXT_CSS_Value * css_vb_values(css_vb * vb, size_t n) {
  if (vb->status != GTEXT_CSS_OK || !vb->set) {
    return NULL;
  }
  if (n == 0) {
    n = 1;
  }
  if (n > vb->max_nodes || vb->nodes > vb->max_nodes - n) {
    css_vb_fail(vb, GTEXT_CSS_E_LIMIT_DECLARATIONS);
    return NULL;
  }
  if (n > SIZE_MAX / sizeof(GTEXT_CSS_Value)) {
    css_vb_fail(vb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  GTEXT_CSS_Value * v = css_arena_alloc(&vb->set->arena, n * sizeof *v);
  if (!v) {
    css_vb_fail(vb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  memset(v, 0, n * sizeof *v);
  vb->nodes += n;
  vb->set->count += n;
  return v;
}

GTEXT_CSS_Value * css_vb_value(css_vb * vb) {
  return css_vb_values(vb, 1);
}

const char * css_vb_str(css_vb * vb, const char * s, size_t n) {
  if (vb->status != GTEXT_CSS_OK || !vb->set) {
    return NULL;
  }
  char * d = css_arena_strdup(&vb->set->arena, s ? s : "", s ? n : 0);
  if (!d) {
    css_vb_fail(vb, GTEXT_CSS_E_OOM);
  }
  return d;
}

bool css_vb_dep(css_vb * vb, const GTEXT_CSS_Value_Set * other) {
  if (vb->status != GTEXT_CSS_OK || !vb->set) {
    return false;
  }
  if (!css_vset_depend(vb->set, other)) {
    css_vb_fail(vb, GTEXT_CSS_E_OOM);
    return false;
  }
  return true;
}

GTEXT_CSS_Value_Set * css_vb_finish(css_vb * vb, const GTEXT_CSS_Value * root) {
  GTEXT_CSS_Value_Set * set = vb->set;
  vb->set = NULL;
  if (vb->status != GTEXT_CSS_OK) {
    gtext_css_value_set_free(set);
    return NULL;
  }
  set->root = root;
  return set;
}

void css_vb_abort(css_vb * vb) {
  gtext_css_value_set_free(vb->set);
  vb->set = NULL;
}

void css_v_keyword(css_vb * vb, GTEXT_CSS_Value * v, const char * text) {
  const size_t n = strlen(text);
  v->kind = GTEXT_CSS_VAL_KEYWORD;
  v->interp = GTEXT_CSS_INTERP_DISCRETE;
  v->text.data = css_vb_str(vb, text, n);
  v->text.length = v->text.data ? n : 0;
}

void css_v_number(GTEXT_CSS_Value * v, double number) {
  v->kind = GTEXT_CSS_VAL_NUMBER;
  v->interp = GTEXT_CSS_INTERP_NUMBER;
  v->number = number;
}

void css_v_integer(GTEXT_CSS_Value * v, double number) {
  v->kind = GTEXT_CSS_VAL_INTEGER;
  v->interp = GTEXT_CSS_INTERP_NUMBER;
  v->number = number;
}

void css_v_percentage(GTEXT_CSS_Value * v, double number) {
  v->kind = GTEXT_CSS_VAL_PERCENTAGE;
  v->interp = GTEXT_CSS_INTERP_PERCENTAGE;
  v->number = number;
}

void css_v_dimension(GTEXT_CSS_Value * v, double number, GTEXT_CSS_Unit unit) {
  v->kind = GTEXT_CSS_VAL_DIMENSION;
  v->number = number;
  v->unit = unit;
  switch (gtext_css_unit_type(unit)) {
  case GTEXT_CSS_UNIT_TYPE_LENGTH: v->interp = GTEXT_CSS_INTERP_LENGTH; break;
  case GTEXT_CSS_UNIT_TYPE_ANGLE: v->interp = GTEXT_CSS_INTERP_ANGLE; break;
  case GTEXT_CSS_UNIT_TYPE_TIME: v->interp = GTEXT_CSS_INTERP_TIME; break;
  default: v->interp = GTEXT_CSS_INTERP_DISCRETE; break;
  }
}

void css_v_list(GTEXT_CSS_Value * v, const GTEXT_CSS_Value * kids, size_t n, GTEXT_CSS_List_Separator sep) {
  v->kind = GTEXT_CSS_VAL_LIST;
  v->interp = GTEXT_CSS_INTERP_LIST;
  v->separator = sep;
  v->children = kids;
  v->child_count = n;
}

void css_v_function(css_vb * vb, GTEXT_CSS_Value * v, const char * name, const GTEXT_CSS_Value * kids, size_t n) {
  const size_t len = strlen(name);
  v->kind = GTEXT_CSS_VAL_FUNCTION;
  v->interp = GTEXT_CSS_INTERP_DISCRETE;
  v->text.data = css_vb_str(vb, name, len);
  v->text.length = v->text.data ? len : 0;
  v->children = kids;
  v->child_count = n;
}
