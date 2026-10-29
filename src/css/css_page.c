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
 * Page selectors (css_page.h): `<page-selector-list> = <page-selector>#`, `<page-selector> = [ <ident-token>? <pseudo-page>* ]!`, `<pseudo-page> = : [ left | right | first | blank ]`,
 * their specificity (Paged Media 3, 4.4) and their match against a page.
 */

#include <string.h>

#include "css_atrule_internal.h"

#define SPEC_CLAMP ((uint32_t)65535)

static uint32_t clamp_add(uint32_t a) {
  return a >= SPEC_CLAMP ? SPEC_CLAMP : a + 1;
}

int gtext_css_page_specificity_compare(GTEXT_CSS_Page_Specificity a, GTEXT_CSS_Page_Specificity b) {
  if (a.f != b.f) {
    return a.f < b.f ? -1 : 1;
  }
  if (a.g != b.g) {
    return a.g < b.g ? -1 : 1;
  }
  if (a.h != b.h) {
    return a.h < b.h ? -1 : 1;
  }
  return 0;
}

bool gtext_css_page_selector_matches(const GTEXT_CSS_Page_Selector * s, const GTEXT_CSS_Page_Context * page) {
  if (!s || !page) {
    return false;
  }
  if (s->name.data) {
    if (!page->name.data || page->name.length != s->name.length || memcmp(page->name.data, s->name.data, s->name.length) != 0) {
      return false; /* "a page type name": the page's `page` property names it */
    }
  }
  if (s->first && !page->first) {
    return false;
  }
  if (s->blank && !page->blank) {
    return false;
  }
  if (s->left && page->side != GTEXT_CSS_PAGE_SIDE_LEFT) {
    return false;
  }
  if (s->right && page->side != GTEXT_CSS_PAGE_SIDE_RIGHT) {
    return false;
  }
  return true;
}

bool gtext_css_page_matches(const GTEXT_CSS_Page_Selector * selectors, size_t count, const GTEXT_CSS_Page_Context * page, GTEXT_CSS_Page_Specificity * specificity) {
  GTEXT_CSS_Page_Specificity best = {0, 0, 0};
  if (!page) {
    return false;
  }
  if (count == 0) {
    if (specificity) {
      *specificity = best;
    }
    return true;
  }
  bool any = false;
  for (size_t i = 0; i < count; i++) {
    if (gtext_css_page_selector_matches(&selectors[i], page)) {
      if (!any || gtext_css_page_specificity_compare(selectors[i].specificity, best) > 0) {
        best = selectors[i].specificity;
      }
      any = true;
    }
  }
  if (any && specificity) {
    *specificity = best;
  }
  return any;
}

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

GTEXT_CSS_Status css_page_selectors_parse(const GTEXT_Allocator * alloc, css_arena * arena, const GTEXT_CSS_Component_Value * values, size_t count, size_t max,
    GTEXT_CSS_Page_Selector ** out, size_t * n, bool * valid) {
  *out = NULL;
  *n = 0;
  *valid = true;
  /* an empty prelude is the empty list: the rule matches every page */
  bool blank = true;
  for (size_t i = 0; i < count; i++) {
    blank = blank && is_ws(&values[i]);
  }
  if (blank) {
    return GTEXT_CSS_OK;
  }
  size_t groups = 1;
  for (size_t i = 0; i < count; i++) {
    if (values[i].kind == GTEXT_CSS_VALUE_TOKEN && values[i].type == GTEXT_CSS_TOKEN_COMMA) {
      groups++;
    }
  }
  if (groups > max) {
    return GTEXT_CSS_E_LIMIT_DESCRIPTORS;
  }
  GTEXT_CSS_Page_Selector * sels = gtext_allocator_calloc(alloc, groups, sizeof *sels);
  if (!sels) {
    return GTEXT_CSS_E_OOM;
  }
  size_t g = 0, start = 0;
  for (;;) {
    size_t end = start;
    while (end < count && !(values[end].kind == GTEXT_CSS_VALUE_TOKEN && values[end].type == GTEXT_CSS_TOKEN_COMMA)) {
      end++;
    }
    GTEXT_CSS_Page_Selector * s = &sels[g++];
    size_t i = start;
    while (i < end && is_ws(&values[i])) {
      i++;
    }
    size_t used = 0;
    if (i < end && values[i].kind == GTEXT_CSS_VALUE_TOKEN && values[i].type == GTEXT_CSS_TOKEN_IDENT) {
      char * nm = css_arena_strdup(arena, values[i].value.data, values[i].value.length);
      if (!nm) {
        gtext_allocator_free(alloc, sels);
        return GTEXT_CSS_E_OOM;
      }
      s->name.data = nm;
      s->name.length = values[i].value.length;
      s->specificity.f = 1;
      i++;
      used++;
    }
    /* the pseudo-pages follow with no white space between them: `:first:left`, `name:first` */
    while (i < end) {
      if (values[i].kind == GTEXT_CSS_VALUE_TOKEN && values[i].type == GTEXT_CSS_TOKEN_COLON && i + 1 < end && values[i + 1].kind == GTEXT_CSS_VALUE_TOKEN &&
          values[i + 1].type == GTEXT_CSS_TOKEN_IDENT) {
        const GTEXT_CSS_String * w = &values[i + 1].value;
        if (css_ascii_ieq(w, "first")) {
          s->first++;
          s->specificity.g = clamp_add(s->specificity.g);
        }
        else if (css_ascii_ieq(w, "blank")) {
          s->blank++;
          s->specificity.g = clamp_add(s->specificity.g);
        }
        else if (css_ascii_ieq(w, "left")) {
          s->left++;
          s->specificity.h = clamp_add(s->specificity.h);
        }
        else if (css_ascii_ieq(w, "right")) {
          s->right++;
          s->specificity.h = clamp_add(s->specificity.h);
        }
        else {
          *valid = false;
          break;
        }
        i += 2;
        used++;
        continue;
      }
      if (is_ws(&values[i])) {
        /* white space after the last component of the selector is allowed, and nothing may follow it */
        size_t k = i;
        while (k < end && is_ws(&values[k])) {
          k++;
        }
        if (k < end) {
          *valid = false;
        }
        break;
      }
      *valid = false;
      break;
    }
    if (!*valid || used == 0) {
      *valid = false;
      gtext_allocator_free(alloc, sels);
      return GTEXT_CSS_OK;
    }
    if (end >= count) {
      break;
    }
    start = end + 1;
  }
  *out = sels;
  *n = groups;
  return GTEXT_CSS_OK;
}
