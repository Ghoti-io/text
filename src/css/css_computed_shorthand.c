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
 * A shorthand written from the computed values of its longhands (css_style.h, gtext_css_computed_shorthand_text()): CSSOM's
 * "serialize a CSS declaration block" over a block that holds the style's computed longhands, by the writer of css_shorthand_serialize.c.
 */

#include "css_compute_internal.h"

#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/css/css_shorthand.h>
#include <ghoti.io/text/css/css_style.h>

#include <string.h>

/// The most leaf longhands under one shorthand that are read (`all` has more, and has no text).
#define MAX_LEAVES 256u

/// Every leaf longhand under `sh` (what it resets included), nested shorthands expanded, without repeats. False past the cap.
static bool collect_leaves(const GTEXT_CSS_Registry * reg, GTEXT_CSS_Property_Id sh, GTEXT_CSS_Property_Id * out, size_t * n) {
  GTEXT_CSS_Property_Id stack[MAX_LEAVES];
  size_t sp = 0;
  *n = 0;
  stack[sp++] = sh;
  while (sp) {
    const GTEXT_CSS_Property_Id cur = stack[--sp];
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(reg, cur, &info)) {
      return false;
    }
    for (size_t i = 0; i < info.longhand_count + info.reset_longhand_count; i++) {
      const GTEXT_CSS_Property_Id raw = i < info.longhand_count ? gtext_css_registry_longhand(reg, cur, i)
                                                                 : gtext_css_registry_reset_longhand(reg, cur, i - info.longhand_count);
      const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(reg, raw);
      GTEXT_CSS_Property_Info li;
      if (!id || !gtext_css_registry_property_info(reg, id, &li)) {
        return false;
      }
      if (li.flags & GTEXT_CSS_PROP_SHORTHAND) {
        if (sp == MAX_LEAVES) {
          return false;
        }
        stack[sp++] = id;
        continue;
      }
      bool seen = false;
      for (size_t k = 0; k < *n && !seen; k++) {
        seen = out[k] == id;
      }
      if (!seen) {
        if (*n == MAX_LEAVES) {
          return false;
        }
        out[(*n)++] = id;
      }
    }
  }
  return true;
}

/// An empty text in a buffer of the kind gtext_css_block_shorthand_value() writes (the default allocator's: release with gtext_css_serialize_buffer_free(NULL, ...)).
static char * empty_text(void) {
  GTEXT_Allocator dflt = *gtext_allocator_default();
  char * e = gtext_allocator_malloc(&dflt, 1);
  if (e) {
    e[0] = '\0';
  }
  return e;
}

GTEXT_CSS_Status gtext_css_computed_shorthand_text(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id shorthand, char ** out, size_t * out_length,
    GTEXT_CSS_Compute_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
  }
  if (!style || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  *out_length = 0;
  const GTEXT_CSS_Registry * reg = style->cc->registry;
  const GTEXT_CSS_Property_Id target = gtext_css_registry_resolve_alias(reg, shorthand);
  GTEXT_CSS_Property_Info ti;
  if (!target || !gtext_css_registry_property_info(reg, target, &ti) || !(ti.flags & GTEXT_CSS_PROP_SHORTHAND)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Property_Id leaves[MAX_LEAVES];
  size_t nleaf = 0;
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &style->cc->alloc;
  if (!collect_leaves(reg, target, leaves, &nleaf)) {
    /* more longhands than a shorthand has (`all`): the longhands are written one by one */
    *out = empty_text();
    return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Declaration_Options bo;
  memset(&bo, 0, sizeof bo);
  bo.allocator = &style->cc->alloc;
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Status st = gtext_css_block_new(reg, &bo, &block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  for (size_t i = 0; i < nleaf && st == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Value_Set * set = NULL;
    st = gtext_css_computed_get(style, leaves[i], &set, result);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    GTEXT_CSS_Property_Info li;
    gtext_css_registry_property_info(reg, leaves[i], &li);
    /* Backgrounds 4, 2.7: the computed value of `background-position` is "a list, each item a pair of offsets (horizontal and vertical) from the top left origin, each
       offset given as a computed <length-percentage> value"; its longhands keep the origin keyword (2.6), so the keyword is folded into the offset here */
    GTEXT_CSS_Value_Set * offsets = NULL;
    if ((li.name_length == 21 && memcmp(li.name, "background-position-x", 21) == 0) || (li.name_length == 21 && memcmp(li.name, "background-position-y", 21) == 0)) {
      st = css_algo_axis_offsets(style->cc, set, &offsets);
      if (st != GTEXT_CSS_OK) {
        break;
      }
      set = offsets;
    }
    char * text = NULL;
    size_t tl = 0;
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
    st = root ? gtext_css_serialize_value_buffer(root, &so, &text, &tl) : GTEXT_CSS_E_INVALID;
    gtext_css_value_set_free(offsets);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    st = gtext_css_block_add_text(block, li.name, li.name_length, text, tl, false, NULL, &outcome, NULL);
    gtext_css_serialize_buffer_free(&so, text);
    if (st == GTEXT_CSS_OK && outcome != GTEXT_CSS_PROP_RESULT_VALID && outcome != GTEXT_CSS_PROP_RESULT_WIDE) {
      /* a computed value that does not read back as the property's own: nothing can be written */
      *out = empty_text();
      gtext_css_block_free(block);
      return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    }
  }
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_block_shorthand_value(block, target, out, out_length, NULL);
  }
  gtext_css_block_free(block);
  return st;
}
