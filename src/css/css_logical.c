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
 * Logical properties (css_logical.h): the abstract-to-physical table of Writing Modes 4, 6.4, and the reading of the name of a member of a
 * logical group.
 */

#include <ghoti.io/text/css/css_logical.h>

#include <string.h>

#include "css_compute_internal.h"

/* ---- the flow --------------------------------------------------------------------------------- */

static bool vertical_mode(GTEXT_CSS_Writing_Mode m) {
  return m != GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB;
}

GTEXT_CSS_Direction gtext_css_flow_used_direction(const GTEXT_CSS_Flow * flow) {
  if (!flow) {
    return GTEXT_CSS_DIRECTION_LTR;
  }
  if ((flow->writing_mode == GTEXT_CSS_WRITING_MODE_VERTICAL_RL || flow->writing_mode == GTEXT_CSS_WRITING_MODE_VERTICAL_LR) &&
      flow->text_orientation == GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT) {
    return GTEXT_CSS_DIRECTION_LTR;
  }
  return flow->direction == GTEXT_CSS_DIRECTION_RTL ? GTEXT_CSS_DIRECTION_RTL : GTEXT_CSS_DIRECTION_LTR;
}

GTEXT_CSS_Side gtext_css_flow_block_side(const GTEXT_CSS_Flow * flow, bool block_end) {
  GTEXT_CSS_Writing_Mode m = flow ? flow->writing_mode : GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB;
  GTEXT_CSS_Side start;
  switch (m) {
  case GTEXT_CSS_WRITING_MODE_VERTICAL_RL:
  case GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL: start = GTEXT_CSS_SIDE_RIGHT; break;
  case GTEXT_CSS_WRITING_MODE_VERTICAL_LR:
  case GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR: start = GTEXT_CSS_SIDE_LEFT; break;
  default: start = GTEXT_CSS_SIDE_TOP; break;
  }
  if (!block_end) {
    return start;
  }
  return (GTEXT_CSS_Side)(((int)start + 2) % 4);
}

GTEXT_CSS_Side gtext_css_flow_inline_side(const GTEXT_CSS_Flow * flow, bool inline_end) {
  GTEXT_CSS_Writing_Mode m = flow ? flow->writing_mode : GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB;
  const bool rtl = gtext_css_flow_used_direction(flow) == GTEXT_CSS_DIRECTION_RTL;
  GTEXT_CSS_Side start;
  if (!vertical_mode(m)) {
    start = rtl ? GTEXT_CSS_SIDE_RIGHT : GTEXT_CSS_SIDE_LEFT;
  }
  else if (m == GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR) {
    start = rtl ? GTEXT_CSS_SIDE_TOP : GTEXT_CSS_SIDE_BOTTOM;
  }
  else {
    start = rtl ? GTEXT_CSS_SIDE_BOTTOM : GTEXT_CSS_SIDE_TOP;
  }
  if (!inline_end) {
    return start;
  }
  return (GTEXT_CSS_Side)(((int)start + 2) % 4);
}

GTEXT_CSS_Axis gtext_css_flow_axis(const GTEXT_CSS_Flow * flow, bool inline_axis) {
  const bool vertical_block = !flow || !vertical_mode(flow->writing_mode);
  /* horizontal-tb: the block axis is vertical and the inline axis horizontal; the vertical modes swap them */
  if (inline_axis) {
    return vertical_block ? GTEXT_CSS_AXIS_HORIZONTAL : GTEXT_CSS_AXIS_VERTICAL;
  }
  return vertical_block ? GTEXT_CSS_AXIS_VERTICAL : GTEXT_CSS_AXIS_HORIZONTAL;
}

const char * gtext_css_writing_mode_name(GTEXT_CSS_Writing_Mode mode) {
  switch (mode) {
  case GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB: return "horizontal-tb";
  case GTEXT_CSS_WRITING_MODE_VERTICAL_RL: return "vertical-rl";
  case GTEXT_CSS_WRITING_MODE_VERTICAL_LR: return "vertical-lr";
  case GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL: return "sideways-rl";
  case GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR: return "sideways-lr";
  default: return NULL;
  }
}

/* ---- the name of a member ------------------------------------------------------------------------- */

static const char * const side_groups[] = {"margin", "padding", "inset", "border-color", "border-style", "border-width", "border-clip",
  "scroll-margin", "scroll-padding", "overflow-clip-margin"};
static const char * const corner_groups[] = {"border-radius", "corner-shape"};
static const char * const axis_groups[] = {"size", "min-size", "max-size", "contain-intrinsic-size", "overflow", "overscroll-behavior",
  "background-position", "background-repeat"};

static GTEXT_CSS_Logical_Kind kind_of_group(const char * group) {
  for (size_t i = 0; i < sizeof side_groups / sizeof *side_groups; i++) {
    if (strcmp(group, side_groups[i]) == 0) {
      return GTEXT_CSS_LOGICAL_SIDES;
    }
  }
  for (size_t i = 0; i < sizeof corner_groups / sizeof *corner_groups; i++) {
    if (strcmp(group, corner_groups[i]) == 0) {
      return GTEXT_CSS_LOGICAL_CORNERS;
    }
  }
  for (size_t i = 0; i < sizeof axis_groups / sizeof *axis_groups; i++) {
    if (strcmp(group, axis_groups[i]) == 0) {
      return GTEXT_CSS_LOGICAL_AXES;
    }
  }
  return GTEXT_CSS_LOGICAL_NONE;
}

/// The words of a name (hyphen separated), at most 8.
static size_t words_of(const char * name, size_t n, const char * w[8], size_t wl[8]) {
  size_t k = 0, start = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i == n || name[i] == '-') {
      if (i > start) {
        if (k == 8) {
          return 9;
        }
        w[k] = name + start;
        wl[k] = i - start;
        k++;
      }
      start = i + 1;
    }
  }
  return k;
}

static bool word_is(const char * w, size_t l, const char * lit) {
  return strlen(lit) == l && memcmp(w, lit, l) == 0;
}

static int side_word(const char * w, size_t l) {
  if (word_is(w, l, "top")) {
    return GTEXT_CSS_SIDE_TOP;
  }
  if (word_is(w, l, "right")) {
    return GTEXT_CSS_SIDE_RIGHT;
  }
  if (word_is(w, l, "bottom")) {
    return GTEXT_CSS_SIDE_BOTTOM;
  }
  if (word_is(w, l, "left")) {
    return GTEXT_CSS_SIDE_LEFT;
  }
  return -1;
}

bool css_logical_describe(const char * group, const char * name, size_t length, css_lmember * out) {
  memset(out, 0, sizeof *out);
  out->kind = (uint8_t)kind_of_group(group);
  if (out->kind == GTEXT_CSS_LOGICAL_NONE) {
    return false;
  }
  const char * w[8];
  size_t wl[8];
  const size_t k = words_of(name, length, w, wl);
  if (k > 8) {
    return false;
  }
  switch (out->kind) {
  case GTEXT_CSS_LOGICAL_SIDES:
    for (size_t i = 0; i < k; i++) {
      if (i + 1 < k && (word_is(w[i], wl[i], "block") || word_is(w[i], wl[i], "inline")) &&
          (word_is(w[i + 1], wl[i + 1], "start") || word_is(w[i + 1], wl[i + 1], "end"))) {
        out->logical = 1;
        out->desc = (uint8_t)((word_is(w[i], wl[i], "inline") ? 2 : 0) + (word_is(w[i + 1], wl[i + 1], "end") ? 1 : 0));
        return true;
      }
      const int s = side_word(w[i], wl[i]);
      if (s >= 0) {
        out->slot = (uint8_t)s;
        return true;
      }
    }
    return false;
  case GTEXT_CSS_LOGICAL_CORNERS:
    for (size_t i = 0; i + 1 < k; i++) {
      const bool a_logical = word_is(w[i], wl[i], "start") || word_is(w[i], wl[i], "end");
      const bool b_logical = word_is(w[i + 1], wl[i + 1], "start") || word_is(w[i + 1], wl[i + 1], "end");
      if (a_logical && b_logical) {
        out->logical = 1;
        out->desc = (uint8_t)((word_is(w[i], wl[i], "end") ? 2 : 0) + (word_is(w[i + 1], wl[i + 1], "end") ? 1 : 0));
        return true;
      }
      const bool a_vert = word_is(w[i], wl[i], "top") || word_is(w[i], wl[i], "bottom");
      const bool b_horiz = word_is(w[i + 1], wl[i + 1], "left") || word_is(w[i + 1], wl[i + 1], "right");
      if (a_vert && b_horiz) {
        const bool bottom = word_is(w[i], wl[i], "bottom");
        const bool right = word_is(w[i + 1], wl[i + 1], "right");
        out->slot = (uint8_t)(bottom ? (right ? GTEXT_CSS_CORNER_BOTTOM_RIGHT : GTEXT_CSS_CORNER_BOTTOM_LEFT)
                                     : (right ? GTEXT_CSS_CORNER_TOP_RIGHT : GTEXT_CSS_CORNER_TOP_LEFT));
        return true;
      }
    }
    return false;
  case GTEXT_CSS_LOGICAL_AXES: {
    const char * last = w[k - 1];
    const size_t ll = wl[k - 1];
    if (word_is(last, ll, "x") || word_is(last, ll, "width")) {
      out->slot = GTEXT_CSS_AXIS_HORIZONTAL;
      return true;
    }
    if (word_is(last, ll, "y") || word_is(last, ll, "height")) {
      out->slot = GTEXT_CSS_AXIS_VERTICAL;
      return true;
    }
    const char * axis = last;
    size_t al = ll;
    if (word_is(last, ll, "size") && k >= 2) {
      axis = w[k - 2];
      al = wl[k - 2];
    }
    if (word_is(axis, al, "block") || word_is(axis, al, "inline")) {
      out->logical = 1;
      out->desc = word_is(axis, al, "inline") ? 1 : 0;
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

size_t css_logical_slot_of(const css_lmember * m, const GTEXT_CSS_Flow * flow) {
  if (!m->logical) {
    return m->slot;
  }
  switch ((GTEXT_CSS_Logical_Kind)m->kind) {
  case GTEXT_CSS_LOGICAL_SIDES:
    return m->desc < 2 ? (size_t)gtext_css_flow_block_side(flow, m->desc == 1) : (size_t)gtext_css_flow_inline_side(flow, m->desc == 3);
  case GTEXT_CSS_LOGICAL_CORNERS: {
    const GTEXT_CSS_Side b = gtext_css_flow_block_side(flow, (m->desc & 2) != 0);
    const GTEXT_CSS_Side i = gtext_css_flow_inline_side(flow, (m->desc & 1) != 0);
    const GTEXT_CSS_Side vertical = (b == GTEXT_CSS_SIDE_TOP || b == GTEXT_CSS_SIDE_BOTTOM) ? b : i;
    const GTEXT_CSS_Side horizontal = (b == GTEXT_CSS_SIDE_LEFT || b == GTEXT_CSS_SIDE_RIGHT) ? b : i;
    if (vertical == GTEXT_CSS_SIDE_TOP) {
      return horizontal == GTEXT_CSS_SIDE_LEFT ? GTEXT_CSS_CORNER_TOP_LEFT : GTEXT_CSS_CORNER_TOP_RIGHT;
    }
    return horizontal == GTEXT_CSS_SIDE_LEFT ? GTEXT_CSS_CORNER_BOTTOM_LEFT : GTEXT_CSS_CORNER_BOTTOM_RIGHT;
  }
  case GTEXT_CSS_LOGICAL_AXES:
    return (size_t)gtext_css_flow_axis(flow, m->desc == 1);
  default:
    return 0;
  }
}

/* ---- the registry -------------------------------------------------------------------------------- */

static GTEXT_CSS_Status member_of(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property, css_lmember * lm,
    const char ** group_out, GTEXT_CSS_Property_Id * resolved) {
  const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(registry, property);
  GTEXT_CSS_Property_Info info;
  if (!id || !gtext_css_registry_property_info(registry, id, &info)) {
    return GTEXT_CSS_E_INVALID;
  }
  if (resolved) {
    *resolved = id;
  }
  memset(lm, 0, sizeof *lm);
  if (!info.logical_group) {
    return GTEXT_CSS_OK;
  }
  if (group_out) {
    *group_out = info.logical_group;
  }
  if (!css_logical_describe(info.logical_group, info.name, info.name_length, lm)) {
    lm->kind = GTEXT_CSS_LOGICAL_NONE;
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_logical_member(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property,
    GTEXT_CSS_Logical_Member * out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  memset(out, 0, sizeof *out);
  if (!registry) {
    return GTEXT_CSS_E_INVALID;
  }
  css_lmember lm;
  const char * group = NULL;
  const GTEXT_CSS_Status st = member_of(registry, property, &lm, &group, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!group) {
    return GTEXT_CSS_OK;
  }
  out->kind = (GTEXT_CSS_Logical_Kind)lm.kind;
  out->group = group;
  out->logical = lm.logical != 0;
  out->slot = lm.logical ? 0 : lm.slot;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_logical_slot(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property, const GTEXT_CSS_Flow * flow,
    GTEXT_CSS_Logical_Kind * kind, size_t * slot) {
  if (!registry || !flow || !slot) {
    return GTEXT_CSS_E_INVALID;
  }
  css_lmember lm;
  const char * group = NULL;
  GTEXT_CSS_Status st = member_of(registry, property, &lm, &group, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!group || lm.kind == GTEXT_CSS_LOGICAL_NONE) {
    return GTEXT_CSS_E_INVALID;
  }
  if (kind) {
    *kind = (GTEXT_CSS_Logical_Kind)lm.kind;
  }
  *slot = css_logical_slot_of(&lm, flow);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_logical_sharers(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Flow * flow, size_t index, size_t * count, GTEXT_CSS_Property_Id * member) {
  if (!registry || !flow) {
    return GTEXT_CSS_E_INVALID;
  }
  if (member) {
    *member = GTEXT_CSS_PROPERTY_NONE;
  }
  css_lmember lm;
  const char * group = NULL;
  GTEXT_CSS_Status st = member_of(registry, property, &lm, &group, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!group || lm.kind == GTEXT_CSS_LOGICAL_NONE) {
    return GTEXT_CSS_E_INVALID;
  }
  const size_t want = css_logical_slot_of(&lm, flow);
  const size_t n = gtext_css_registry_logical_group_size(group, strlen(group));
  size_t found = 0;
  for (size_t i = 0; i < n; i++) {
    const GTEXT_CSS_Property_Id m = gtext_css_registry_logical_group_member(registry, group, strlen(group), i);
    GTEXT_CSS_Property_Info mi;
    css_lmember ml;
    if (!m || !gtext_css_registry_property_info(registry, m, &mi) || !css_logical_describe(group, mi.name, mi.name_length, &ml)) {
      continue;
    }
    if (css_logical_slot_of(&ml, flow) == want) {
      if (found == index && member) {
        *member = m;
      }
      found++;
    }
  }
  if (count) {
    *count = found;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_logical_map(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property, const GTEXT_CSS_Flow * flow,
    GTEXT_CSS_Property_Id * physical) {
  if (!registry || !flow || !physical) {
    return GTEXT_CSS_E_INVALID;
  }
  css_lmember lm;
  const char * group = NULL;
  GTEXT_CSS_Property_Id id = 0;
  GTEXT_CSS_Status st = member_of(registry, property, &lm, &group, &id);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!group || lm.kind == GTEXT_CSS_LOGICAL_NONE) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!lm.logical) {
    *physical = id;
    return GTEXT_CSS_OK;
  }
  const size_t want = css_logical_slot_of(&lm, flow);
  const size_t n = gtext_css_registry_logical_group_size(group, strlen(group));
  for (size_t i = 0; i < n; i++) {
    const GTEXT_CSS_Property_Id m = gtext_css_registry_logical_group_member(registry, group, strlen(group), i);
    GTEXT_CSS_Property_Info mi;
    css_lmember ml;
    if (!m || !gtext_css_registry_property_info(registry, m, &mi) || !css_logical_describe(group, mi.name, mi.name_length, &ml)) {
      continue;
    }
    if (!ml.logical && ml.slot == want) {
      *physical = m;
      return GTEXT_CSS_OK;
    }
  }
  return GTEXT_CSS_E_UNSUPPORTED;
}
