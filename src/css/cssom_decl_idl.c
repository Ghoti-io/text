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
 * The IDL attribute names of CSSStyleDeclaration (css_cssom_decl.h): a binary search over the generated table (cssom_tables.h, tools/css/cssom_idl_names.tsv), and
 * the CSSOM algorithm "CSS property to IDL attribute" for the other direction.
 */

#include <string.h>

#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_internal.h"
#include "cssom_tables.h"

static int row_compare(const char * attribute, size_t length, const char * name) {
  const size_t n = strlen(name);
  const size_t m = length < n ? length : n;
  const int c = m ? memcmp(attribute, name, m) : 0;
  if (c) {
    return c;
  }
  return length < n ? -1 : length > n ? 1 : 0;
}

bool css_idl_lookup(const char * attribute, size_t length, const char ** property, size_t * property_length) {
  if (!attribute) {
    return false;
  }
  size_t lo = 0, hi = cssom_idl_row_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int c = row_compare(attribute, length, cssom_idl_rows[mid].attribute);
    if (c == 0) {
      if (property) {
        *property = cssom_idl_rows[mid].property;
      }
      if (property_length) {
        *property_length = strlen(cssom_idl_rows[mid].property);
      }
      return true;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return false;
}

bool gtext_cssom_idl_property(const char * attribute, size_t length, const char ** property, size_t * property_length) {
  return css_idl_lookup(attribute, length, property, property_length);
}

GTEXT_CSS_Status gtext_cssom_decl_idl_get(GTEXT_CSSOM_Decl * decl, const char * attribute, size_t length, char ** out, size_t * out_length) {
  const char * property = NULL;
  size_t n = 0;
  if (!decl || !css_idl_lookup(attribute, length, &property, &n)) {
    return GTEXT_CSS_E_INVALID;
  }
  return gtext_cssom_decl_get_property_value(decl, property, n, out, out_length);
}

GTEXT_CSS_Status gtext_cssom_decl_idl_set(GTEXT_CSSOM_Decl * decl, const char * attribute, size_t length, const char * value, size_t value_length,
    GTEXT_CSSOM_Outcome * outcome) {
  const char * property = NULL;
  size_t n = 0;
  if (!decl || !css_idl_lookup(attribute, length, &property, &n)) {
    return GTEXT_CSS_E_INVALID;
  }
  return gtext_cssom_decl_set_property(decl, property, n, value, value_length, "", 0, outcome);
}

GTEXT_CSS_Status gtext_cssom_idl_attribute(const char * property, size_t length, const GTEXT_Allocator * allocator, char ** out, size_t * out_length) {
  if (!property || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_Allocator a = *(allocator ? allocator : gtext_allocator_default());
  char * s = gtext_allocator_malloc(&a, length + 1);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  bool uppercase_next = false;
  for (size_t i = 0; i < length; i++) {
    char c = property[i];
    if (c == '-') {
      uppercase_next = true;
    }
    else if (uppercase_next) {
      uppercase_next = false;
      s[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    else {
      s[n++] = c;
    }
  }
  s[n] = '\0';
  *out = s;
  if (out_length) {
    *out_length = n;
  }
  return GTEXT_CSS_OK;
}
