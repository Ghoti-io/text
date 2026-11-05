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
 * The comparison of a style range (Conditional 5, "evaluate a <style-range>"), shared by the `style()` of `@container` and of `if()`. Private.
 *
 * "3. Parse <style-range-value> to <number>, <percentage>, <length>, <angle>, <time>, <frequency> or <resolution>. If this cannot be done, evaluate to false. 4. If each
 * <style-range-value> from the range have the same type, compute each and evaluate the comparison; a unitless zero must be treated as being a zero-<length> when compared
 * against a <length>. 5. Otherwise evaluate to false." The caller makes each operand a computed value (a number, a percentage or a dimension in absolute units); this file
 * reads such a token and compares.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_STYLE_RANGE_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_STYLE_RANGE_H

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/css/css_value.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The comparators of a range, as written: `<`, `<=`, `>`, `>=`, `=`.
typedef enum { CSR_LT = 1, CSR_LE, CSR_GT, CSR_GE, CSR_EQ } css_range_cmp;

/// An operand read: a number, a percentage, or a dimension (its value in the canonical unit of its type).
typedef struct {
  double v;
  GTEXT_CSS_Unit_Type type;  ///< the unit's type; NONE for a number or a percentage
  bool percentage;
  bool number;
} css_range_operand;

/// Read a token as an operand: a number, a percentage, or a dimension of a unit that converts to its canonical unit (an absolute length, angle, time, frequency or
/// resolution). false: it is none of those (a relative unit that was not computed, a flex, a decibel, an identifier, ...).
bool css_range_operand_from_token(const GTEXT_CSS_Component_Value * tok, css_range_operand * op);

/// Whether two operands are of the same type for a comparison; a unitless zero is a zero length against a length.
bool css_range_comparable(const css_range_operand * a, const css_range_operand * b);

/// `a <op> b` for operands that are comparable.
bool css_range_compare(const css_range_operand * a, css_range_cmp op, const css_range_operand * b);

#ifdef __cplusplus
}
#endif

#endif
