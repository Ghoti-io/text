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
 * Basic shapes (CSS Shapes 1) and the geometry boxes of Masking 1.
 *
 * Reference: CSS Shapes Module Level 1, Editor's Draft of 8 October 2026 (served 2026-10-22); CSS Masking Level 1.
 *
 * The grammars are the drafts' own (Value Definition Syntax), retrieved 2026-10-22; where the
 * served draft differs from an older text the served one is followed and the difference is noted
 * beside the entry.
 * **Implicitly omitted commas are written out.** Values 4, 2.2 omits a comma from the match when the terms around it are omitted
 * (`path(\"M 0 0\")` for `path( <'fill-rule'>? , <string> )`). The engine keeps a literal comma that separates a possibly empty
 * term as a token, so the grammars here say it with a group (`path( [ <'fill-rule'> , ]? <string> )`), which matches the same inputs.
 *
 * They are matched by the Value Definition Syntax engine; nothing here is code.
 */

#include "css_function_internal.h"

static const css_fn_type TYPES[] = {
  /* Shapes 1 */
  {"basic-shape", GTEXT_CSS_REF_TYPE,
      "<basic-shape-rect> | <circle()> | <ellipse()> | <polygon()> | <path()> | <shape()>"},
  {"basic-shape-rect", GTEXT_CSS_REF_TYPE,
      "<inset()> | <xywh()> | <rect()>"},
  {"inset()", GTEXT_CSS_REF_TYPE,
      "inset( <length-percentage>{1,4} [ round <'border-radius'> ]? )"},
  {"xywh()", GTEXT_CSS_REF_TYPE,
      "xywh( <length-percentage>{2} <length-percentage [0,∞]>{2} [ round <'border-radius'> ]? )"},
  {"rect()", GTEXT_CSS_REF_TYPE,
      "rect( [ <length-percentage> | auto ]{4} [ round <'border-radius'> ]? )"},
  /* Shapes 1 (served 2026-10-22) uses <radial-size> */
  {"circle()", GTEXT_CSS_REF_TYPE,
      "circle( <radial-size>? [ at <position> ]? )"},
  {"ellipse()", GTEXT_CSS_REF_TYPE,
      "ellipse( <radial-size>? [ at <position> ]? )"},
  {"polygon()", GTEXT_CSS_REF_TYPE,
      "polygon( [ [ <'fill-rule'> [ round <length> ]? | round <length> ] , ]? [ <length-percentage> <length-percentage> ]# )"},
  {"path()", GTEXT_CSS_REF_TYPE,
      "path( [ <'fill-rule'> , ]? <string> )"},
  {"shape()", GTEXT_CSS_REF_TYPE,
      "shape( <'fill-rule'>? from <position> , <shape-command># )"},
  {"shape-command", GTEXT_CSS_REF_TYPE,
      "<move-command> | <line-command> | close | <horizontal-line-command> | <vertical-line-command> | <curve-command> | <smooth-command> | <arc-command>"},
  {"move-command", GTEXT_CSS_REF_TYPE,
      "move <command-end-point>"},
  {"line-command", GTEXT_CSS_REF_TYPE,
      "line <command-end-point>"},
  {"horizontal-line-command", GTEXT_CSS_REF_TYPE,
      "hline [ to [ <length-percentage> | left | center | right | x-start | x-end ] | by <length-percentage> ]"},
  {"vertical-line-command", GTEXT_CSS_REF_TYPE,
      "vline [ to [ <length-percentage> | top | center | bottom | y-start | y-end ] | by <length-percentage> ]"},
  {"curve-command", GTEXT_CSS_REF_TYPE,
      "curve [ [ to <position> with <control-point> [ / <control-point> ]? ] | [ by <coordinate-pair> with <relative-control-point> [ / <relative-control-point> ]? ] ]"},
  {"smooth-command", GTEXT_CSS_REF_TYPE,
      "smooth [ [ to <position> [ with <control-point> ]? ] | [ by <coordinate-pair> [ with <relative-control-point> ]? ] ]"},
  {"arc-command", GTEXT_CSS_REF_TYPE,
      "arc <command-end-point> [ [ of <length-percentage>{1,2} ] && <arc-sweep>? && <arc-size>? && [ rotate <angle> ]? ]"},
  {"command-end-point", GTEXT_CSS_REF_TYPE,
      "[ to <position> | by <coordinate-pair> ]"},
  {"control-point", GTEXT_CSS_REF_TYPE,
      "[ <position> | <relative-control-point> ]"},
  {"relative-control-point", GTEXT_CSS_REF_TYPE,
      "<coordinate-pair> [ from [ start | end | origin ] ]?"},
  {"coordinate-pair", GTEXT_CSS_REF_TYPE,
      "<length-percentage>{2}"},
  {"arc-sweep", GTEXT_CSS_REF_TYPE,
      "cw | ccw"},
  {"arc-size", GTEXT_CSS_REF_TYPE,
      "large | small"},
  /* Box Model / Masking 1 */
  {"visual-box", GTEXT_CSS_REF_TYPE,
      "content-box | padding-box | border-box"},
  /* Shapes 1 */
  {"shape-box", GTEXT_CSS_REF_TYPE,
      "<visual-box> | margin-box | half-border-box"},
  /* Masking 1 */
  {"geometry-box", GTEXT_CSS_REF_TYPE,
      "<shape-box> | fill-box | stroke-box | view-box"},
  /* Fill and Stroke 3 / Masking: the property the shape functions name; the generated registry (C1c) has it too and, chained first, wins */
  {"fill-rule", GTEXT_CSS_REF_PROPERTY,
      "nonzero | evenodd"},
  /* Backgrounds 3: the property `round` takes; the generated registry wins when chained first */
  {"border-radius", GTEXT_CSS_REF_PROPERTY,
      "<length-percentage [0,∞]>{1,4} [ / <length-percentage [0,∞]>{1,4} ]?"},
};

const css_fn_type * css_shape_types(size_t * count) {
  *count = sizeof TYPES / sizeof TYPES[0];
  return TYPES;
}

#include <string.h>

/* ---- the typed view ------------------------------------------------------------------------------ */

GTEXT_CSS_Status gtext_css_shape_from_value(const GTEXT_CSS_Value * v, const GTEXT_Allocator * allocator, bool * valid, GTEXT_CSS_Shape * out) {
  if (!v || !valid || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  *valid = false;
  memset(out, 0, sizeof *out);
  if (v->kind != GTEXT_CSS_VAL_FUNCTION || v->math) {
    return GTEXT_CSS_OK;
  }
  static const struct { const char * n; GTEXT_CSS_Shape_Kind k; } K[] = {
    {"inset", GTEXT_CSS_SHAPE_INSET}, {"xywh", GTEXT_CSS_SHAPE_XYWH}, {"rect", GTEXT_CSS_SHAPE_RECT}, {"circle", GTEXT_CSS_SHAPE_CIRCLE},
    {"ellipse", GTEXT_CSS_SHAPE_ELLIPSE}, {"polygon", GTEXT_CSS_SHAPE_POLYGON}, {"path", GTEXT_CSS_SHAPE_PATH}, {"shape", GTEXT_CSS_SHAPE_SHAPE},
  };
  GTEXT_CSS_Shape_Kind kind = GTEXT_CSS_SHAPE_NONE;
  for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
    if (css_fn_name_is(&v->text, K[i].n)) {
      kind = K[i].k;
    }
  }
  if (kind == GTEXT_CSS_SHAPE_NONE) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_Allocator * a = allocator ? allocator : gtext_allocator_default();
  css_leafvec lv;
  if (!css_fn_leaves(a, v, &lv)) {
    return GTEXT_CSS_E_OOM;
  }
  out->kind = kind;
  for (size_t i = 0; i < lv.n; i++) {
    const GTEXT_CSS_Value * l = lv.v[i];
    if (!l) {
      continue;
    }
    if (css_fn_leaf_is_keyword(l, "evenodd")) {
      out->even_odd = true;
      out->fill_rule_written = true;
    }
    else if (css_fn_leaf_is_keyword(l, "nonzero")) {
      out->fill_rule_written = true;
    }
    else if (css_fn_leaf_is_keyword(l, "round")) {
      out->has_round = true;
    }
    else if (kind == GTEXT_CSS_SHAPE_PATH && l->kind == GTEXT_CSS_VAL_STRING) {
      out->path = l;
    }
  }
  css_fn_leaves_free(a, &lv);
  *valid = true;
  return GTEXT_CSS_OK;
}
