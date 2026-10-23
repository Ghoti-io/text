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
 * The units of CSS Values 4 and 5: their names, what they measure, and the
 * fixed ratios of the absolute ones.
 *
 * Reference: CSS Values and Units Module Level 4, Editor's Draft, section 6
 * (Distance Units, Angle Units, Time Units, Frequency Units, Resolution Units)
 * and CSS Grid for `fr`; Level 5 and Containment 3 for the container units.
 */

#include <ghoti.io/text/macros.h>

#include <math.h>
#include <string.h>

#include "css_grammar_internal.h"

/// One unit: its spelling, kind, canonical unit and the ratio to it.
typedef struct {
  GTEXT_CSS_Unit unit;          ///< The unit.
  const char * name;            ///< Canonical spelling.
  GTEXT_CSS_Unit_Type type;     ///< What it measures.
  GTEXT_CSS_Unit canonical;     ///< The unit of its type that others convert to.
  bool absolute;                ///< A fixed ratio to `canonical`.
  double numerator;             ///< The quantity in the canonical unit is `value * numerator / denominator`.
  double denominator;
} unit_row;

#define LEN_ABS(u, n, num, den) {GTEXT_CSS_UNIT_##u, n, GTEXT_CSS_UNIT_TYPE_LENGTH, GTEXT_CSS_UNIT_PX, true, num, den}
#define LEN_REL(u, n) {GTEXT_CSS_UNIT_##u, n, GTEXT_CSS_UNIT_TYPE_LENGTH, GTEXT_CSS_UNIT_PX, false, 1.0, 1.0}

/* 1in = 2.54cm = 25.4mm = 101.6Q = 72pt = 6pc = 96px (Values 4, 6.2). */
static const unit_row kUnits[] = {
  LEN_ABS(CM, "cm", 9600.0, 254.0), /* 96 / 2.54 */
  LEN_ABS(MM, "mm", 960.0, 254.0), /* 96 / 25.4 */
  LEN_ABS(Q, "Q", 960.0, 1016.0), /* 96 / 101.6 */
  LEN_ABS(IN, "in", 96.0, 1.0),
  LEN_ABS(PC, "pc", 16.0, 1.0),
  LEN_ABS(PT, "pt", 4.0, 3.0), /* 96 / 72 */
  LEN_ABS(PX, "px", 1.0, 1.0),
  LEN_REL(EM, "em"), LEN_REL(REM, "rem"), LEN_REL(EX, "ex"), LEN_REL(REX, "rex"),
  LEN_REL(CAP, "cap"), LEN_REL(RCAP, "rcap"), LEN_REL(CH, "ch"), LEN_REL(RCH, "rch"),
  LEN_REL(IC, "ic"), LEN_REL(RIC, "ric"), LEN_REL(LH, "lh"), LEN_REL(RLH, "rlh"),
  LEN_REL(VW, "vw"), LEN_REL(SVW, "svw"), LEN_REL(LVW, "lvw"), LEN_REL(DVW, "dvw"),
  LEN_REL(VH, "vh"), LEN_REL(SVH, "svh"), LEN_REL(LVH, "lvh"), LEN_REL(DVH, "dvh"),
  LEN_REL(VI, "vi"), LEN_REL(SVI, "svi"), LEN_REL(LVI, "lvi"), LEN_REL(DVI, "dvi"),
  LEN_REL(VB, "vb"), LEN_REL(SVB, "svb"), LEN_REL(LVB, "lvb"), LEN_REL(DVB, "dvb"),
  LEN_REL(VMIN, "vmin"), LEN_REL(SVMIN, "svmin"), LEN_REL(LVMIN, "lvmin"), LEN_REL(DVMIN, "dvmin"),
  LEN_REL(VMAX, "vmax"), LEN_REL(SVMAX, "svmax"), LEN_REL(LVMAX, "lvmax"), LEN_REL(DVMAX, "dvmax"),
  LEN_REL(CQW, "cqw"), LEN_REL(CQH, "cqh"), LEN_REL(CQI, "cqi"), LEN_REL(CQB, "cqb"),
  LEN_REL(CQMIN, "cqmin"), LEN_REL(CQMAX, "cqmax"),
  /* 1turn = 360deg = 400grad = 2 pi rad (6.3) */
  {GTEXT_CSS_UNIT_DEG, "deg", GTEXT_CSS_UNIT_TYPE_ANGLE, GTEXT_CSS_UNIT_DEG, true, 1.0, 1.0},
  {GTEXT_CSS_UNIT_GRAD, "grad", GTEXT_CSS_UNIT_TYPE_ANGLE, GTEXT_CSS_UNIT_DEG, true, 360.0, 400.0},
  {GTEXT_CSS_UNIT_RAD, "rad", GTEXT_CSS_UNIT_TYPE_ANGLE, GTEXT_CSS_UNIT_DEG, true, 180.0, 3.14159265358979323846},
  {GTEXT_CSS_UNIT_TURN, "turn", GTEXT_CSS_UNIT_TYPE_ANGLE, GTEXT_CSS_UNIT_DEG, true, 360.0, 1.0},
  /* 1s = 1000ms (6.4) */
  {GTEXT_CSS_UNIT_S, "s", GTEXT_CSS_UNIT_TYPE_TIME, GTEXT_CSS_UNIT_S, true, 1.0, 1.0},
  {GTEXT_CSS_UNIT_MS, "ms", GTEXT_CSS_UNIT_TYPE_TIME, GTEXT_CSS_UNIT_S, true, 1.0, 1000.0},
  /* 1kHz = 1000Hz (6.5) */
  {GTEXT_CSS_UNIT_HZ, "Hz", GTEXT_CSS_UNIT_TYPE_FREQUENCY, GTEXT_CSS_UNIT_HZ, true, 1.0, 1.0},
  {GTEXT_CSS_UNIT_KHZ, "kHz", GTEXT_CSS_UNIT_TYPE_FREQUENCY, GTEXT_CSS_UNIT_HZ, true, 1000.0, 1.0},
  /* 1dppx = 1x = 96dpi = 96/2.54 dpcm (6.6): the canonical unit is dppx */
  {GTEXT_CSS_UNIT_DPI, "dpi", GTEXT_CSS_UNIT_TYPE_RESOLUTION, GTEXT_CSS_UNIT_DPPX, true, 1.0, 96.0},
  {GTEXT_CSS_UNIT_DPCM, "dpcm", GTEXT_CSS_UNIT_TYPE_RESOLUTION, GTEXT_CSS_UNIT_DPPX, true, 254.0, 9600.0},
  {GTEXT_CSS_UNIT_DPPX, "dppx", GTEXT_CSS_UNIT_TYPE_RESOLUTION, GTEXT_CSS_UNIT_DPPX, true, 1.0, 1.0},
  {GTEXT_CSS_UNIT_X, "x", GTEXT_CSS_UNIT_TYPE_RESOLUTION, GTEXT_CSS_UNIT_DPPX, true, 1.0, 1.0},
  {GTEXT_CSS_UNIT_FR, "fr", GTEXT_CSS_UNIT_TYPE_FLEX, GTEXT_CSS_UNIT_FR, true, 1.0, 1.0},
};

#define UNIT_ROWS (sizeof kUnits / sizeof kUnits[0])

/* A row for each unit, by its enumerator, built once (the table above is the order of the rows, not of the enum). */
static const unit_row * row_of(GTEXT_CSS_Unit unit) {
  /* The rows are few (63): a scan is as cheap as an index and cannot go out of step with the enum. */
  if ((unsigned)unit == 0 || (unsigned)unit >= (unsigned)GTEXT_CSS_UNIT_COUNT) {
    return NULL;
  }
  for (size_t i = 0; i < UNIT_ROWS; i++) {
    if (kUnits[i].unit == unit) {
      return &kUnits[i];
    }
  }
  return NULL;
}

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

GTEXT_CSS_Unit gtext_css_unit_lookup(const char * name, size_t length) {
  if (!name || length == 0 || length > 5) {
    return GTEXT_CSS_UNIT_NONE;
  }
  for (size_t i = 0; i < UNIT_ROWS; i++) {
    const char * n = kUnits[i].name;
    size_t k = 0;
    while (k < length && n[k] && lower(n[k]) == lower(name[k])) {
      k++;
    }
    if (k == length && n[k] == '\0') {
      return kUnits[i].unit;
    }
  }
  return GTEXT_CSS_UNIT_NONE;
}

const char * gtext_css_unit_name(GTEXT_CSS_Unit unit) {
  const unit_row * r = row_of(unit);
  return r ? r->name : NULL;
}

GTEXT_CSS_Unit_Type gtext_css_unit_type(GTEXT_CSS_Unit unit) {
  const unit_row * r = row_of(unit);
  return r ? r->type : GTEXT_CSS_UNIT_TYPE_NONE;
}

bool gtext_css_unit_is_absolute(GTEXT_CSS_Unit unit) {
  const unit_row * r = row_of(unit);
  return r && r->absolute;
}

GTEXT_CSS_Unit gtext_css_unit_canonical(GTEXT_CSS_Unit unit) {
  const unit_row * r = row_of(unit);
  return r ? r->canonical : GTEXT_CSS_UNIT_NONE;
}

bool gtext_css_unit_to_canonical(GTEXT_CSS_Unit unit, double value, double * out) {
  const unit_row * r = row_of(unit);
  if (!out || !r || !r->absolute) {
    return false;
  }
  if (r->numerator == 1.0 && r->denominator == 1.0) {
    *out = value;
  }
  else {
    *out = value * r->numerator / r->denominator;
  }
  return true;
}
