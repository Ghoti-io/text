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

#include "css_style_range.h"

#include <string.h>

bool css_range_operand_from_token(const GTEXT_CSS_Component_Value * tok, css_range_operand * op) {
  memset(op, 0, sizeof *op);
  if (tok->kind != GTEXT_CSS_VALUE_TOKEN) {
    return false;
  }
  switch (tok->type) {
  case GTEXT_CSS_TOKEN_NUMBER:
    op->v = tok->number;
    op->number = true;
    return true;
  case GTEXT_CSS_TOKEN_PERCENTAGE:
    op->v = tok->number;
    op->percentage = true;
    return true;
  case GTEXT_CSS_TOKEN_DIMENSION: {
    const GTEXT_CSS_Unit u = gtext_css_unit_lookup(tok->unit.data, tok->unit.length);
    double canon = 0;
    if (u == GTEXT_CSS_UNIT_NONE || !gtext_css_unit_to_canonical(u, tok->number, &canon)) {
      return false;
    }
    const GTEXT_CSS_Unit_Type t = gtext_css_unit_type(u);
    if (t != GTEXT_CSS_UNIT_TYPE_LENGTH && t != GTEXT_CSS_UNIT_TYPE_ANGLE && t != GTEXT_CSS_UNIT_TYPE_TIME && t != GTEXT_CSS_UNIT_TYPE_FREQUENCY &&
        t != GTEXT_CSS_UNIT_TYPE_RESOLUTION) {
      return false;  /* the draft names these five types */
    }
    op->v = canon;
    op->type = t;
    return true;
  }
  default:
    return false;
  }
}

bool css_range_comparable(const css_range_operand * a, const css_range_operand * b) {
  if (a->number == b->number && a->percentage == b->percentage && a->type == b->type) {
    return true;
  }
  const bool zero_a = a->number && a->v == 0 && b->type == GTEXT_CSS_UNIT_TYPE_LENGTH;
  const bool zero_b = b->number && b->v == 0 && a->type == GTEXT_CSS_UNIT_TYPE_LENGTH;
  return zero_a || zero_b;
}

bool css_range_compare(const css_range_operand * a, css_range_cmp op, const css_range_operand * b) {
  switch (op) {
  case CSR_LT: return a->v < b->v;
  case CSR_LE: return a->v <= b->v;
  case CSR_GT: return a->v > b->v;
  case CSR_GE: return a->v >= b->v;
  default: return a->v == b->v;
  }
}
