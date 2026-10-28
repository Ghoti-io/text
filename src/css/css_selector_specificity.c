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
 * The specificity of a compiled selector with `&` standing for a given specificity, what a list asks of the caller's element interface, and the dependency
 * summary (css_selector.h).
 */

#include <string.h>

#include "css_selector_internal.h"

static GTEXT_CSS_Specificity spec_zero(void) {
  return gtext_css_specificity_make(0, 0, 0);
}

static GTEXT_CSS_Specificity list_spec(const GTEXT_CSS_Selector_List * l, GTEXT_CSS_Specificity nesting) {
  if (!l) {
    return spec_zero();
  }
  if (!l->has_nesting) {
    return l->max_specificity;
  }
  GTEXT_CSS_Specificity m = spec_zero();
  for (size_t i = 0; i < l->count; i++) {
    const GTEXT_CSS_Specificity c = gtext_css_complex_specificity(&l->complex[i], nesting);
    if (gtext_css_specificity_compare(c, m) > 0) {
      m = c;
    }
  }
  return m;
}

GTEXT_CSS_Specificity css_selector_simple_specificity(const GTEXT_CSS_Simple * s, GTEXT_CSS_Specificity nesting) {
  switch (s->kind) {
  case GTEXT_CSS_SIMPLE_TYPE: return gtext_css_specificity_make(0, 0, 1);
  case GTEXT_CSS_SIMPLE_UNIVERSAL: return spec_zero();
  case GTEXT_CSS_SIMPLE_ID: return gtext_css_specificity_make(1, 0, 0);
  case GTEXT_CSS_SIMPLE_CLASS:
  case GTEXT_CSS_SIMPLE_ATTRIBUTE: return gtext_css_specificity_make(0, 1, 0);
  case GTEXT_CSS_SIMPLE_NESTING: return nesting;
  case GTEXT_CSS_SIMPLE_PSEUDO_CLASS:
  case GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT: break;
  }
  const sel_row * r = css_sel_row_of(s);
  switch ((sel_spec)r->spec) {
  case SELSPEC_CLASS: return gtext_css_specificity_make(0, 1, 0);
  case SELSPEC_ELEMENT: return gtext_css_specificity_make(0, 0, 1);
  case SELSPEC_ID: return gtext_css_specificity_make(1, 0, 0);
  case SELSPEC_ZERO: return spec_zero();
  case SELSPEC_MAX: return list_spec(s->list, nesting);
  case SELSPEC_NTH: return gtext_css_specificity_add(gtext_css_specificity_make(0, 1, 0), s->has_of ? list_spec(s->list, nesting) : spec_zero());
  case SELSPEC_HOSTFN: return gtext_css_specificity_add(gtext_css_specificity_make(0, 1, 0), list_spec(s->list, nesting));
  case SELSPEC_SLOTTED: return gtext_css_specificity_add(gtext_css_specificity_make(0, 0, 1), list_spec(s->list, nesting));
  case SELSPEC_NESTING: return nesting;
  }
  return spec_zero();
}

GTEXT_CSS_Specificity gtext_css_complex_specificity(const GTEXT_CSS_Complex * complex, GTEXT_CSS_Specificity nesting) {
  if (!complex) {
    return spec_zero();
  }
  if (!complex->has_nesting) {
    return complex->specificity;
  }
  GTEXT_CSS_Specificity sp = spec_zero();
  for (size_t i = 0; i < complex->compound_count; i++) {
    const GTEXT_CSS_Compound * cmp = &complex->compounds[i];
    for (size_t k = 0; k < cmp->simple_count; k++) {
      sp = gtext_css_specificity_add(sp, css_selector_simple_specificity(&cmp->simples[k], nesting));
    }
  }
  return sp;
}

GTEXT_CSS_Specificity gtext_css_selector_list_specificity(const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Specificity nesting) {
  return list_spec(list, nesting);
}

void gtext_css_selector_requires(const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Selector_Requires * out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof *out);
  if (!list) {
    return;
  }
  out->callbacks = list->callbacks;
  out->state = list->state;
  out->unmatchable = list->unmatchable;
}

GTEXT_CSS_Status gtext_css_selector_check_interface(const GTEXT_CSS_Element_Interface * iface, const GTEXT_CSS_Selector_List * list,
    GTEXT_CSS_Element_Result * result) {
  GTEXT_CSS_Element_Callback need[GTEXT_CSS_ELEMENT_CB_COUNT];
  size_t n = 0;
  if (!list) {
    if (result) {
      memset(result, 0, sizeof *result);
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  for (int cb = 0; cb < (int)GTEXT_CSS_ELEMENT_CB_COUNT; cb++) {
    if (list->callbacks & (1ull << cb)) {
      need[n++] = (GTEXT_CSS_Element_Callback)cb;
    }
  }
  return gtext_css_element_check(iface, n ? need : NULL, n, result);
}

void gtext_css_selector_dependencies(const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Selector_Dependencies * out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof *out);
  if (!list) {
    return;
  }
  if (list->internal) {
    *out = list->internal->dep;
    return;
  }
  out->kinds = list->dependencies;
  out->state = list->state;
}

static const GTEXT_CSS_String any_namespace = {"*", 1};

const GTEXT_CSS_String * gtext_css_namespace_any(void) {
  return &any_namespace;
}
