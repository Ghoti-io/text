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
 * Property-level parsing: the whole-property rules over the registry and the grammar engine (see css_property.h).
 */

#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/css/css_property.h>
#include <ghoti.io/text/macros.h>

#include <string.h>

#include "css_property_canon.h"
#include "css_registry_internal.h"
#include "css_value_internal.h"

#define PROP_NAME_STACK 4096u

static const struct {
  const char * name;
  GTEXT_CSS_Wide_Keyword keyword;
} kWide[] = {
  {"initial", GTEXT_CSS_WIDE_INITIAL},
  {"inherit", GTEXT_CSS_WIDE_INHERIT},
  {"unset", GTEXT_CSS_WIDE_UNSET},
  {"revert", GTEXT_CSS_WIDE_REVERT},
  {"revert-layer", GTEXT_CSS_WIDE_REVERT_LAYER},
  {"revert-rule", GTEXT_CSS_WIDE_REVERT_RULE},
};

GTEXT_CSS_Wide_Keyword gtext_css_wide_keyword_lookup(const char * name, size_t length) {
  if (!name) {
    return GTEXT_CSS_WIDE_NONE;
  }
  for (size_t i = 0; i < sizeof kWide / sizeof kWide[0]; i++) {
    if (strlen(kWide[i].name) != length) {
      continue;
    }
    size_t k = 0;
    for (; k < length; k++) {
      unsigned char c = (unsigned char)name[k];
      if (c >= 'A' && c <= 'Z') {
        c = (unsigned char)(c + ('a' - 'A'));
      }
      if (c != (unsigned char)kWide[i].name[k]) {
        break;
      }
    }
    if (k == length) {
      return kWide[i].keyword;
    }
  }
  return GTEXT_CSS_WIDE_NONE;
}

const char * gtext_css_wide_keyword_name(GTEXT_CSS_Wide_Keyword keyword) {
  for (size_t i = 0; i < sizeof kWide / sizeof kWide[0]; i++) {
    if (kWide[i].keyword == keyword) {
      return kWide[i].name;
    }
  }
  return NULL;
}

void gtext_css_property_value_free(GTEXT_CSS_Property_Value * value) {
  if (!value) {
    return;
  }
  gtext_css_value_set_free(value->set);
  memset(value, 0, sizeof *value);
}

/** The CSS-wide keyword a value list is, if it is exactly one identifier (white space around it is nothing). */
static GTEXT_CSS_Wide_Keyword wide_of(const GTEXT_CSS_Component_Value * values, size_t count) {
  const GTEXT_CSS_Component_Value * only = NULL;
  for (size_t i = 0; i < count; i++) {
    if (values[i].kind == GTEXT_CSS_VALUE_TOKEN && values[i].type == GTEXT_CSS_TOKEN_WHITESPACE) {
      continue;
    }
    if (only) {
      return GTEXT_CSS_WIDE_NONE;
    }
    only = &values[i];
  }
  if (!only || only->kind != GTEXT_CSS_VALUE_TOKEN || only->type != GTEXT_CSS_TOKEN_IDENT) {
    return GTEXT_CSS_WIDE_NONE;
  }
  return gtext_css_wide_keyword_lookup(only->value.data, only->value.length);
}

static bool is_all(const GTEXT_CSS_Property_Info * info) {
  return (info->flags & GTEXT_CSS_PROP_GENERATED) && info->name_length == 3 && memcmp(info->name, "all", 3) == 0;
}

/** Match against a compiled grammar and fold the answer into the result. */
/* Anchor Positioning 1: "anchor() ... Only valid in the inset properties" and anchor-size() "in the sizing, inset and margin properties", the longhands and the
 * shorthands that set them; the calculations of any other property take neither. */
static unsigned anchor_functions_of(const char * name, size_t length) {
  static const char * const inset[] = {"top", "right", "bottom", "left", "inset-block-start", "inset-block-end", "inset-inline-start", "inset-inline-end",
    "inset", "inset-block", "inset-inline"};
  static const char * const other[] = {"width", "height", "min-width", "min-height", "max-width", "max-height", "block-size", "inline-size", "min-block-size",
    "min-inline-size", "max-block-size", "max-inline-size", "margin", "margin-top", "margin-right", "margin-bottom", "margin-left", "margin-block",
    "margin-inline", "margin-block-start", "margin-block-end", "margin-inline-start", "margin-inline-end"};
  for (size_t i = 0; i < sizeof inset / sizeof *inset; i++) {
    if (strlen(inset[i]) == length && memcmp(inset[i], name, length) == 0) {
      return GTEXT_CSS_MATH_ANCHOR | GTEXT_CSS_MATH_ANCHOR_SIZE;
    }
  }
  for (size_t i = 0; i < sizeof other / sizeof *other; i++) {
    if (strlen(other[i]) == length && memcmp(other[i], name, length) == 0) {
      return GTEXT_CSS_MATH_ANCHOR_SIZE;
    }
  }
  return 0;
}

static bool is_logical_inset_longhand(const GTEXT_CSS_Property_Info * info) {
  static const char * const names[] = {"inset-block-start", "inset-block-end", "inset-inline-start", "inset-inline-end"};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    if (strlen(names[i]) == info->name_length && memcmp(names[i], info->name, info->name_length) == 0) {
      return true;
    }
  }
  return false;
}

static GTEXT_CSS_Status run_match(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Compiled * compiled,
    const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Property_Options * options, bool unitless, bool name_is_clip,
    unsigned anchors, const char * property, size_t property_length, GTEXT_CSS_Property_Value * out) {
  GTEXT_CSS_Match_Options mo = options && options->match ? *options->match : *css_registry_match_options(registry);
  mo.allocator = options && options->allocator ? options->allocator : css_registry_allocator(registry);
  mo.unitless_length = unitless || (options && options->unitless_lengths); /* attribute mode: every property that takes a length */
  mo.unitless_length_nested = unitless && name_is_clip;
  mo.anchor_functions = anchors;
  mo.property = property;
  mo.property_length = property_length;
  bool matched = false;
  GTEXT_CSS_Value_Set * set = NULL;
  const GTEXT_CSS_Status st = gtext_css_match(compiled, values, count, &mo, &matched, &set, &out->match);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (matched) {
    out->outcome = GTEXT_CSS_PROP_RESULT_VALID;
    out->set = set;
  }
  else {
    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
    gtext_css_value_set_free(set);
  }
  return GTEXT_CSS_OK;
}

static bool name_is(const GTEXT_CSS_Component_Value * v, const char * lower) {
  const size_t n = strlen(lower);
  if (v->value.length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)v->value.data[i];
    if (c >= 'A' && c <= 'Z') {
      c = (unsigned char)(c + ('a' - 'A'));
    }
    if (c != (unsigned char)lower[i]) {
      return false;
    }
  }
  return true;
}

/* ---- -webkit-perspective and -webkit-transform: a number is a length --------------------------------------------------------------- */

/// Whether `v` is a white space token.
static bool compat_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

/// Whether `v` is a number token that is a length of that many pixels in the two prefixed properties: not negative (`<number [0,∞]>`).
static bool compat_number(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_NUMBER && v->number >= 0;
}

/// The one value inside `values` that is not white space, or NULL when there is not exactly one.
static const GTEXT_CSS_Component_Value * compat_sole(const GTEXT_CSS_Component_Value * values, size_t count) {
  const GTEXT_CSS_Component_Value * only = NULL;
  for (size_t i = 0; i < count; i++) {
    if (compat_ws(&values[i])) {
      continue;
    }
    if (only) {
      return NULL;
    }
    only = &values[i];
  }
  return only;
}

/// A number token as the same number of pixels: `1000` as `1000px`.
static void compat_to_px(GTEXT_CSS_Component_Value * v) {
  v->type = GTEXT_CSS_TOKEN_DIMENSION;
  v->unit.data = "px";
  v->unit.length = 2;
}

/**
 * The Compatibility Standard gives `-webkit-perspective` the grammar `none | <length [0,∞]> | <number [0,∞]>` and the `perspective()` of `-webkit-transform` a `<number [0,∞]>` beside
 * its length, "a number is pixels"; Chromium 156 and Firefox 158 accept `-webkit-perspective: 1000` and `-webkit-transform: perspective(1000)` and write `1000px` and
 * `perspective(1000px)`. The property is the alias of `perspective` (`transform`), whose grammar has no number, so the number is made the length it stands for before the match.
 * `mode` 1 is `-webkit-perspective` (a value that is a number alone), 2 is `-webkit-transform` (the argument of a `perspective()` that is a number alone). Anything else, a
 * calculation of numbers included (Chromium accepts `calc(1)` there and Firefox does not), is left to the grammar. On success `*owned` is the copy to match and to free
 * (NULL when nothing changed).
 */
static GTEXT_CSS_Status compat_numbers(const GTEXT_Allocator * al, unsigned mode, const GTEXT_CSS_Component_Value * values, size_t count, GTEXT_CSS_Component_Value ** owned) {
  *owned = NULL;
  size_t extra = 0;
  bool any = false;
  if (mode == 1) {
    const GTEXT_CSS_Component_Value * only = compat_sole(values, count);
    any = only && compat_number(only);
  }
  else {
    for (size_t i = 0; i < count; i++) {
      const GTEXT_CSS_Component_Value * v = &values[i];
      if (v->kind == GTEXT_CSS_VALUE_FUNCTION && name_is(v, "perspective")) {
        const GTEXT_CSS_Component_Value * a = compat_sole(v->children, v->child_count);
        if (a && compat_number(a)) {
          any = true;
          extra += v->child_count;
        }
      }
    }
  }
  if (!any) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Component_Value * copy = gtext_allocator_malloc(al, (count + extra) * sizeof *copy);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(copy, values, count * sizeof *copy);
  size_t tail = count;
  if (mode == 1) {
    for (size_t i = 0; i < count; i++) {
      if (compat_number(&copy[i])) {
        compat_to_px(&copy[i]);
      }
    }
  }
  else {
    for (size_t i = 0; i < count; i++) {
      GTEXT_CSS_Component_Value * v = &copy[i];
      if (v->kind != GTEXT_CSS_VALUE_FUNCTION || !name_is(v, "perspective")) {
        continue;
      }
      const GTEXT_CSS_Component_Value * a = compat_sole(v->children, v->child_count);
      if (!a || !compat_number(a)) {
        continue;
      }
      GTEXT_CSS_Component_Value * kids = copy + tail;
      memcpy(kids, v->children, v->child_count * sizeof *kids);
      tail += v->child_count;
      for (size_t k = 0; k < v->child_count; k++) {
        if (compat_number(&kids[k])) {
          compat_to_px(&kids[k]);
        }
      }
      v->children = kids;
    }
  }
  *owned = copy;
  return GTEXT_CSS_OK;
}


static GTEXT_CSS_Status parse_pending(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Value * out) {
  const GTEXT_CSS_Compiled * pending = NULL;
  GTEXT_CSS_Status st = css_registry_pending_grammar(registry, &pending);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = run_match(registry, pending, values, count, options, false, false, 0, NULL, 0, out);
  if (st == GTEXT_CSS_OK && out->outcome == GTEXT_CSS_PROP_RESULT_VALID) {
    out->outcome = GTEXT_CSS_PROP_RESULT_PENDING;
  }
  return st;
}

/** Sets `*fall_through` when the name is no descriptor of an at-rule that also takes properties: the caller then parses it as a property. */
static GTEXT_CSS_Status parse_descriptor(const GTEXT_CSS_Registry * registry, const char * name, size_t name_length,
    const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Property_Options * options,
    GTEXT_CSS_Property_Value * out, bool * fall_through) {
  *fall_through = false;
  const char * at = options->at_rule;
  size_t at_length = strlen(at);
  char at_buf[PROP_NAME_STACK];
  if (at_length > 0 && at[0] != '@') {
    if (at_length + 1 > sizeof at_buf) {
      out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
      return GTEXT_CSS_OK;
    }
    at_buf[0] = '@';
    memcpy(at_buf + 1, at, at_length);
    at = at_buf;
    at_length++;
  }
  const GTEXT_CSS_At_Rule_Id rule = gtext_css_registry_find_at_rule(at, at_length);
  const GTEXT_CSS_Descriptor_Id desc = rule ? gtext_css_registry_find_descriptor(rule, name, name_length) : 0;
  GTEXT_CSS_Descriptor_Info info;
  if (!desc || !gtext_css_registry_descriptor_info(desc, &info) || info.kind == GTEXT_CSS_DESC_AT_RULE) {
    GTEXT_CSS_At_Rule_Info ai;
    *fall_through = rule && gtext_css_registry_at_rule_info(rule, &ai) && ai.properties && !(desc && info.kind == GTEXT_CSS_DESC_AT_RULE);
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    return GTEXT_CSS_OK;
  }
  out->descriptor = desc;
  /* a descriptor is not a property of an element: no CSS-wide keyword and no substitution applies to it */
  bool sub = false;
  GTEXT_CSS_Status st = gtext_css_has_arbitrary_substitution(values, count,
      options->allocator ? options->allocator : css_registry_allocator(registry), &sub);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (sub) {
    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Grammar_State state;
  const GTEXT_CSS_Compiled * compiled = NULL;
  st = css_registry_descriptor_compiled(registry, desc, &state, &compiled, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (state != GTEXT_CSS_GRAMMAR_OK) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  return run_match(registry, compiled, values, count, options, false, false, 0, NULL, 0, out);
}

/// Whether a property writes a bare `0` as a number although its grammar offers a length first: the outsets of a border image and of a mask border
/// (`[ <length [0,∞]> | <number [0,∞]> ]{1,4}`), where the number multiplies the border width and `0` is what authors and browsers write.
static bool zero_is_number_for(const GTEXT_CSS_Property_Info * info) {
  return (info->name_length == 19 && memcmp(info->name, "border-image-outset", 19) == 0) ||
      (info->name_length == 18 && memcmp(info->name, "mask-border-outset", 18) == 0);
}

/// A bare `0` where such a property takes a number as well as a length is the number, so it is written `0`, not `0px`. The tree is the one just matched
/// and belongs to the caller; nothing else shares it.
static void zero_lengths_to_numbers(GTEXT_CSS_Property_Value * out, const GTEXT_CSS_Property_Info * info) {
  if (!out->set || !zero_is_number_for(info)) {
    return;
  }
  GTEXT_CSS_Value * root = (GTEXT_CSS_Value *)gtext_css_value_set_root(out->set);
  struct { GTEXT_CSS_Value * node; size_t i; } stack[64];
  size_t sp = 0;
  if (!root) {
    return;
  }
  stack[sp].node = root;
  stack[sp].i = 0;
  sp++;
  while (sp) {
    GTEXT_CSS_Value * node = stack[sp - 1].node;
    if (stack[sp - 1].i == 0) {
      if (node->kind == GTEXT_CSS_VAL_DIMENSION && (node->flags & GTEXT_CSS_VFLAG_ZERO_LENGTH) && !(node->flags & GTEXT_CSS_VFLAG_MATH)) {
        node->kind = GTEXT_CSS_VAL_NUMBER;
        node->unit = GTEXT_CSS_UNIT_NONE;
        node->interp = GTEXT_CSS_INTERP_NUMBER;
        node->flags &= ~(unsigned)GTEXT_CSS_VFLAG_ZERO_LENGTH;
      }
    }
    if (node->kind != GTEXT_CSS_VAL_LIST || stack[sp - 1].i >= node->child_count) {
      sp--;
      continue;
    }
    GTEXT_CSS_Value * kid = (GTEXT_CSS_Value *)&node->children[stack[sp - 1].i++];
    if (sp < 64) {
      stack[sp].node = kid;
      stack[sp].i = 0;
      sp++;
    }
  }
}

/// Whether a steps() in the component values has a step count that is a constant calculation of NaN. Values 4, 10.9.2: "NaN does not escape a top-level calculation; it's
/// censored into a zero value", and Easing 2, 2.3: "the <integer> must be at least 2 ... or the function is invalid. Otherwise, the <integer> must be at least 1": zero is
/// neither. (A finite count below the least is clamped when computed, Values 4, 10.12; both pinned browsers refuse the NaN.)
static bool steps_count_is_nan(const GTEXT_CSS_Component_Value * values, size_t count) {
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Component_Value * v = &values[i];
    if (v->kind != GTEXT_CSS_VALUE_FUNCTION) {
      continue;
    }
    if (v->value.length == 5 && (v->value.data[0] | 32) == 's' && (v->value.data[1] | 32) == 't' && (v->value.data[2] | 32) == 'e' && (v->value.data[3] | 32) == 'p' &&
        (v->value.data[4] | 32) == 's') {
      for (size_t k = 0; k < v->child_count; k++) {
        const GTEXT_CSS_Component_Value * c = &v->children[k];
        if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_WHITESPACE) {
          continue;
        }
        if (c->kind == GTEXT_CSS_VALUE_FUNCTION && gtext_css_math_is_function(c->value.data, c->value.length)) {
          GTEXT_CSS_Math * math = NULL;
          bool valid = false;
          if (gtext_css_math_parse(c, NULL, NULL, &valid, &math, NULL) == GTEXT_CSS_OK && valid) {
            double n = 0.0;
            const bool nan = gtext_css_math_numeric(math, &n, NULL, NULL) && n != n;
            gtext_css_math_free(math);
            if (nan) {
              return true;
            }
          }
        }
        break;
      }
    }
    else if (v->child_count && steps_count_is_nan(v->children, v->child_count)) {
      return true;
    }
  }
  return false;
}

/// Whether a `transition` value has more than one comma-separated item and one of them has `none` as its property: Transitions 1, 2.3, "If there is more than one
/// <single-transition> in the shorthand, and any of the transitions has none as the <single-transition-property>, then the declaration is invalid."
static bool transition_none_in_list(const GTEXT_CSS_Component_Value * values, size_t count) {
  size_t items = 1;
  bool none = false;
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Component_Value * v = &values[i];
    if (v->kind != GTEXT_CSS_VALUE_TOKEN) {
      continue;
    }
    if (v->type == GTEXT_CSS_TOKEN_COMMA) {
      items++;
    }
    else if (v->type == GTEXT_CSS_TOKEN_IDENT && css_ascii_ieq(&v->value, "none")) {
      none = true;
    }
  }
  return items > 1 && none;
}

/// Whether, in some comma-separated item of a `transition` or `animation` value, the first component value that is a <time> is negative: it is the duration then.
static bool first_time_is_negative(const GTEXT_CSS_Component_Value * values, size_t count) {
  bool seen = false;
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Component_Value * v = &values[i];
    if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_COMMA) {
      seen = false;
      continue;
    }
    if (seen) {
      continue;
    }
    if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_DIMENSION) {
      const GTEXT_CSS_Unit u = gtext_css_unit_lookup(v->unit.data, v->unit.length);
      if (u != GTEXT_CSS_UNIT_NONE && gtext_css_unit_type(u) == GTEXT_CSS_UNIT_TYPE_TIME) {
        if (v->number < 0) {
          return true;
        }
        seen = true;
      }
    }
    else if (v->kind == GTEXT_CSS_VALUE_FUNCTION && gtext_css_math_is_function(v->value.data, v->value.length)) {
      GTEXT_CSS_Math * math = NULL;
      bool valid = false;
      if (gtext_css_math_parse(v, NULL, NULL, &valid, &math, NULL) != GTEXT_CSS_OK || !valid) {
        continue;
      }
      if (math->category == GTEXT_CSS_MATH_CAT_TIME) {
        double n = 0.0;
        const bool numeric = gtext_css_math_numeric(math, &n, NULL, NULL);
        gtext_css_math_free(math);
        if (numeric && n < 0) {
          return true;
        }
        seen = true;
      }
      else {
        gtext_css_math_free(math);
      }
    }
  }
  return false;
}

/// Whether a `flex` value gives a unitless number to the basis that Flexbox 1 gives to a flex factor: "A unitless zero that is not already preceded by two flex factors must be
/// interpreted as a flex factor." So `0 1 1` is grow 0, shrink 1 and a `1` for the basis, which is not a length: the declaration is invalid, where the grammar's `||` would read it as
/// basis `0` with grow 1 and shrink 1 (both pinned browsers refuse it, css/css-flexbox/getcomputedstyle/flexbox_computedstyle_flex-shorthand-invalid.html).
static bool flex_number_is_basis(const GTEXT_CSS_Component_Value * values, size_t count) {
  size_t factors = 0;
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Component_Value * v = &values[i];
    if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE) {
      continue;
    }
    if (v->kind != GTEXT_CSS_VALUE_TOKEN) {
      return false;   /* a function or block: not judged here */
    }
    if (v->type == GTEXT_CSS_TOKEN_NUMBER) {
      if (factors < 2) {
        factors++;
      }
      else if (v->number != 0.0) {
        return true;   /* a third number is the basis, and a number other than zero is not a length */
      }
    }
  }
  return false;
}

GTEXT_CSS_Status gtext_css_parse_property(const GTEXT_CSS_Registry * registry, const char * name, size_t name_length,
    const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Property_Options * options,
    GTEXT_CSS_Property_Value * out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  memset(out, 0, sizeof *out);
  out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  if (!registry || !name || (!values && count != 0)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Property_Options defaults;
  memset(&defaults, 0, sizeof defaults);
  if (!options) {
    options = &defaults;
  }
  GTEXT_CSS_Status st;
  if (options->at_rule) {
    bool fall_through = false;
    st = parse_descriptor(registry, name, name_length, values, count, options, out, &fall_through);
    if (st != GTEXT_CSS_OK) {
      gtext_css_property_value_free(out);
      out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    }
    if (st != GTEXT_CSS_OK || !fall_through) {
      return st;
    }
  }

  const GTEXT_CSS_Property_Id named = gtext_css_registry_find_property(registry, name, name_length);
  if (!named) {
    return GTEXT_CSS_OK;
  }
  out->named = named;
  const GTEXT_CSS_Property_Id target = gtext_css_registry_resolve_alias(registry, named);
  out->property = target;
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(registry, target, &info)) {
    return GTEXT_CSS_E_INTERNAL;
  }

  /* CSS-wide keywords are valid alone for every property */
  const GTEXT_CSS_Wide_Keyword wide = wide_of(values, count);
  if (wide != GTEXT_CSS_WIDE_NONE) {
    out->outcome = GTEXT_CSS_PROP_RESULT_WIDE;
    out->wide = wide;
    return GTEXT_CSS_OK;
  }
  bool sub = false;
  st = gtext_css_has_arbitrary_substitution(values, count,
      options->allocator ? options->allocator : css_registry_allocator(registry), &sub);
  if (st != GTEXT_CSS_OK) {
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    return st;
  }
  /* `all` takes nothing else but a CSS-wide keyword, which a function may yield: `all: var(--x)` is pending, and is invalid at computed-value time unless the
     functions are replaced by a keyword (a browser accepts it too) */
  if (is_all(&info) && !sub) {
    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
    return GTEXT_CSS_OK;
  }
  if (sub) {
    bool syntax = true;
    st = css_substitution_syntax(values, count, options->allocator ? options->allocator : css_registry_allocator(registry), &syntax);
    if (st != GTEXT_CSS_OK) {
      out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
      return st;
    }
    if (!syntax) {
      out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
      return GTEXT_CSS_OK;
    }
    st = parse_pending(registry, values, count, options, out);
    if (st != GTEXT_CSS_OK) {
      gtext_css_property_value_free(out);
      out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    }
    return st;
  }
  GTEXT_CSS_Grammar_State state;
  const GTEXT_CSS_Compiled * compiled = NULL;
  /* Anchor Positioning 1 (served 2026-11-01): "It is only allowed in the inset properties", and "These are only usable in the inset properties ... or the logical
     inset properties that refer to the horizontal axis"; the pinned data gives the four logical inset longhands `auto | <length-percentage>` (its Position 3 entry has no
     extension), where `inset`, `inset-block` and `inset-inline` take `<'top'>`. The longhands are read with the grammar of `top`, which is the one the pin gives the physical ones. */
  GTEXT_CSS_Property_Id grammar_of = target;
  if (is_logical_inset_longhand(&info)) {
    const GTEXT_CSS_Property_Id top = gtext_css_registry_find_property(registry, "top", 3);
    if (top) {
      grammar_of = top;
    }
  }
  st = css_registry_property_compiled(registry, grammar_of, &state, &compiled, NULL);
  if (st != GTEXT_CSS_OK) {
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    return st;
  }
  if (state != GTEXT_CSS_GRAMMAR_OK) {
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  /* the two prefixed names the Compatibility Standard gives a number to (see compat_numbers()) */
  unsigned compat_mode = 0;
  GTEXT_CSS_Component_Value * compat_values = NULL;
  const GTEXT_Allocator * match_alloc = options->allocator ? options->allocator : css_registry_allocator(registry);
  if (named != target) {
    GTEXT_CSS_Property_Info named_info;
    if (gtext_css_registry_property_info(registry, named, &named_info) && (named_info.flags & GTEXT_CSS_PROP_ALIAS) && named_info.name_length >= 11 &&
        memcmp(named_info.name, "-webkit-", 8) == 0) {
      if (named_info.name_length == 19 && memcmp(named_info.name, "-webkit-perspective", 19) == 0) {
        compat_mode = 1;
      }
      else if (named_info.name_length == 17 && memcmp(named_info.name, "-webkit-transform", 17) == 0) {
        compat_mode = 2;
      }
    }
  }
  if (compat_mode) {
    st = compat_numbers(match_alloc, compat_mode, values, count, &compat_values);
    if (st != GTEXT_CSS_OK) {
      out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
      return st;
    }
  }
  st = run_match(registry, compiled, compat_values ? compat_values : values, count, options,
      options->quirks && (info.flags & GTEXT_CSS_PROP_QUIRK_UNITLESS) != 0, info.name_length == 4 && memcmp(info.name, "clip", 4) == 0,
      anchor_functions_of(info.name, info.name_length), info.name, info.name_length, out);
  gtext_allocator_free(match_alloc, compat_values);
  if (st != GTEXT_CSS_OK) {
    gtext_css_property_value_free(out);
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  }
  else if (out->outcome == GTEXT_CSS_PROP_RESULT_VALID && info.name_length == 4 && memcmp(info.name, "flex", 4) == 0 && flex_number_is_basis(values, count)) {
    gtext_css_value_set_free(out->set);
    out->set = NULL;
    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
    return GTEXT_CSS_OK;
  }
  else if (out->outcome == GTEXT_CSS_PROP_RESULT_VALID && info.name_length >= 9 &&
           (memcmp(info.name, "transition", 10) == 0 || memcmp(info.name, "animation", 9) == 0) &&
           ((first_time_is_negative(values, count) && (info.name_length == 10 || info.name_length == 9)) || steps_count_is_nan(values, count) ||
            (info.name_length == 10 && transition_none_in_list(values, count)))) {
    /* Transitions 1, 2.3 (and Animations 1 of animation): "the first value that can be parsed as a time is assigned to the transition-duration, and the second value that can be
       parsed as a time is assigned to transition-delay", and a duration is "<time [0,∞]>" ("Negative values for transition-duration are invalid"): `-1s -2s`, and `-2s 1s`, which the
       grammar's `||` reads with the times swapped, are not a transition */
    gtext_css_value_set_free(out->set);
    out->set = NULL;
    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
    return GTEXT_CSS_OK;
  }
  else if (out->outcome == GTEXT_CSS_PROP_RESULT_VALID) {
    /* Color HDR 1, 3.3: the mixing percentages of dynamic-range-limit-mix() are "normalized to sum to 100%", which a sum of 0% cannot be: both pinned browsers refuse it */
    if (info.name_length == 19 && memcmp(info.name, "dynamic-range-limit", 19) == 0) {
      const GTEXT_Allocator * al = options->allocator ? options->allocator : css_registry_allocator(registry);
      GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
      so.allocator = al;
      char * text = NULL;
      size_t text_length = 0;
      bool zero = false;
      GTEXT_CSS_Status ss = gtext_css_serialize_values_buffer(values, count, &so, &text, &text_length);
      if (ss == GTEXT_CSS_OK) {
        ss = css_mix_sums_to_zero(al, text, text_length, &zero) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
        gtext_css_serialize_buffer_free(&so, text);
      }
      if (ss != GTEXT_CSS_OK) {
        gtext_css_property_value_free(out);
        out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
        return ss;
      }
      if (zero) {
        gtext_css_value_set_free(out->set);
        out->set = NULL;
        out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
        return GTEXT_CSS_OK;
      }
    }
    out->canonical = css_canon_rules_of(info.name, info.name_length);
    zero_lengths_to_numbers(out, &info);
  }
  return st;
}

/// A custom property's value is the text it was written as: the root of its value (a valid or a pending one) takes it over from the serialization of the tokens.
void css_trim_value_text(const char ** s, size_t * n) {
  const char * t = *s;
  const size_t len = *n;
  size_t first = len, last = 0; /* the first byte of the first token, one past the last byte of the last */
  size_t i = 0;
  while (i < len) {
    const char c = t[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      i++;
    }
    else if (c == '/' && i + 1 < len && t[i + 1] == '*') {
      size_t j = i + 2;
      while (j + 1 < len && !(t[j] == '*' && t[j + 1] == '/')) {
        j++;
      }
      i = j + 1 < len ? j + 2 : len; /* an unterminated comment runs to the end */
    }
    else {
      if (first == len) {
        first = i;
      }
      if (c == '"' || c == '\'') {
        size_t j = i + 1;
        while (j < len && t[j] != c) {
          j += t[j] == '\\' && j + 1 < len ? 2 : 1;
        }
        i = j < len ? j + 1 : len;
      }
      else if (c == '\\' && i + 1 < len) {
        i += 2;
      }
      else {
        i++;
      }
      last = i;
    }
  }
  if (first == len) {
    *s = t + len;
    *n = 0;
    return;
  }
  *s = t + first;
  *n = last - first;
}

static GTEXT_CSS_Status use_custom_text(GTEXT_CSS_Property_Value * out, const char * text, size_t length) {
  if (!out->set || (out->outcome != GTEXT_CSS_PROP_RESULT_VALID && out->outcome != GTEXT_CSS_PROP_RESULT_PENDING)) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value_Set * set = out->set;
  GTEXT_CSS_Value * root = (GTEXT_CSS_Value *)set->root;
  if (!root) {
    return GTEXT_CSS_OK;
  }
  char * copy = css_arena_strdup(&set->arena, text, length);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  root->text.data = copy;
  root->text.length = length;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_property_value_keep_text(GTEXT_CSS_Property_Value * out, const char * text, size_t length) {
  return use_custom_text(out, text, length);
}

bool css_property_value_text(const GTEXT_CSS_Property_Value * value, const char ** text, size_t * length) {
  if (!value->set || (value->outcome != GTEXT_CSS_PROP_RESULT_VALID && value->outcome != GTEXT_CSS_PROP_RESULT_PENDING) || !value->set->root || !value->set->root->text.data) {
    return false;
  }
  *text = value->set->root->text.data;
  *length = value->set->root->text.length;
  return true;
}

GTEXT_CSS_Status gtext_css_parse_property_text(const GTEXT_CSS_Registry * registry, const char * name, size_t name_length,
    const char * text, size_t text_length, const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Value * out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  memset(out, 0, sizeof *out);
  out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  if (!registry || !name || (!text && text_length != 0)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = options && options->allocator ? options->allocator : css_registry_allocator(registry);
  GTEXT_CSS_Tree * tree = NULL;
  if (name_length == 13) {
    static const char kUr[] = "unicode-range";
    size_t k = 0;
    for (; k < 13; k++) {
      if ((name[k] | 0x20) != kUr[k]) {
        break;
      }
    }
    if (k == 13) {
      /* The tokenizer makes unicode-range tokens only in the value of a `unicode-range` declaration (CSS Syntax 3: "consume the value of a
       * unicode-range descriptor"), so the text is read as that declaration's value. */
      const size_t total = 14 + text_length;
      char * decl = gtext_allocator_malloc(po.allocator, total + 1);
      if (!decl) {
        return GTEXT_CSS_E_OOM;
      }
      memcpy(decl, "unicode-range:", 14);
      if (text_length) {
        memcpy(decl + 14, text, text_length);
      }
      decl[total] = '\0';
      GTEXT_CSS_Status ds = gtext_css_parse_declaration_list(decl, total, &po, &tree, NULL);
      gtext_allocator_free(po.allocator, decl);
      if (ds != GTEXT_CSS_OK) {
        return ds;
      }
      if (tree->declaration_count != 1 || tree->rule_count != 0) {
        gtext_css_tree_free(tree);
        out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
        return GTEXT_CSS_OK;  /* more than one declaration: the text was not one value */
      }
      const GTEXT_CSS_Declaration * d = &tree->declarations[0];
      ds = gtext_css_parse_property(registry, name, name_length, d->value, d->value_count, options, out);
      gtext_css_tree_free(tree);
      return ds;
    }
  }
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text ? text : "", text_length, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  /* Syntax 3, "consume a declaration": "remove leading and trailing <whitespace-token>s from the value"; a value read from text is a declaration's value */
  const GTEXT_CSS_Component_Value * first = tree->values;
  size_t kept = tree->value_count;
  while (kept && first->kind == GTEXT_CSS_VALUE_TOKEN && first->type == GTEXT_CSS_TOKEN_WHITESPACE) {
    first++;
    kept--;
  }
  while (kept && first[kept - 1].kind == GTEXT_CSS_VALUE_TOKEN && first[kept - 1].type == GTEXT_CSS_TOKEN_WHITESPACE) {
    kept--;
  }
  st = gtext_css_parse_property(registry, name, name_length, first, kept, options, out);
  gtext_css_tree_free(tree);
  if (st == GTEXT_CSS_OK && out->outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
    /* Values 5 (served 2026-11-01): "the shorthand property must serialize to that original (arbitrary substitution function-containing) value", and both pinned browsers
       write the text of a value that holds a substitution function as it was written (white space inside it, the quotes of its strings), trimmed at both ends. */
    const char * t = text ? text : "";
    size_t tl = text_length;
    css_trim_value_text(&t, &tl);
    /* only a text the tokenizer reads back as itself: a control character (a NUL, a form feed, a carriage return ...) is changed by the preprocessing of the input stream
       (CSS Syntax 3, "preprocess the input stream"), and the text of the tokens is the one that is stable */
    bool plain = true;
    for (size_t i = 0; plain && i < tl; i++) {
      const unsigned char ch = (unsigned char)t[i];
      plain = ch >= 0x20 || ch == '\t' || ch == '\n';
      plain = plain && ch != 0x7f;
    }
    st = plain ? use_custom_text(out, t, tl) : GTEXT_CSS_OK;
    if (st != GTEXT_CSS_OK) {
      gtext_css_property_value_free(out);
      out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    }
  }
  if (st == GTEXT_CSS_OK && name_length >= 2 && name[0] == '-' && name[1] == '-') {
    /* a custom property's value is its text "exactly as specified by the author": comments and the spelling of its numbers are kept (Variables 1, 4.1) */
    const char * t = text ? text : "";
    size_t tl = text_length;
    css_trim_value_text(&t, &tl);
    st = use_custom_text(out, t, tl);
    if (st != GTEXT_CSS_OK) {
      gtext_css_property_value_free(out);
      out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    }
  }
  return st;
}

GTEXT_CSS_Status gtext_css_parse_property_declaration(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Declaration * declaration,
    const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Value * out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!declaration) {
    memset(out, 0, sizeof *out);
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = gtext_css_parse_property(registry, declaration->name.data, declaration->name.length, declaration->value,
      declaration->value_count, options, out);
  const GTEXT_CSS_String * written = declaration->original_text.data ? &declaration->original_text : &declaration->substitution_text;
  if (st == GTEXT_CSS_OK && written->data && (declaration->original_text.data || out->outcome == GTEXT_CSS_PROP_RESULT_PENDING)) {
    const char * ot = written->data;
    size_t ol = written->length;
    css_trim_value_text(&ot, &ol);
    st = use_custom_text(out, ot, ol);
    if (st != GTEXT_CSS_OK) {
      gtext_css_property_value_free(out);
      out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    }
  }
  return st;
}

/* ---- serialization ------------------------------------------------------------------------ */

/** The value written in its canonical form (css_property_canon.c): the typed value is serialized as it was matched and the text is rewritten. */
static GTEXT_CSS_Status serialize_canonical(const GTEXT_CSS_Property_Value * value, const GTEXT_CSS_Serialize_Options * options,
    const GTEXT_CSS_Value * root, char ** out, size_t * out_length) {
  char * raw = NULL;
  size_t raw_length = 0;
  GTEXT_CSS_Status st = gtext_css_serialize_value_buffer(root, options, &raw, &raw_length);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_Allocator a = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
  st = css_canon_apply(value->canonical, &a, raw, raw_length, out, out_length);
  gtext_css_serialize_buffer_free(options, raw);
  return st;
}

GTEXT_CSS_Status gtext_css_property_value_serialize(const GTEXT_CSS_Property_Value * value, const GTEXT_CSS_Serialize_Options * options,
    const GTEXT_Markup_Sink * sink) {
  if (!value || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  switch (value->outcome) {
  case GTEXT_CSS_PROP_RESULT_WIDE: {
    const char * k = gtext_css_wide_keyword_name(value->wide);
    if (!k) {
      return GTEXT_CSS_E_INVALID;
    }
    return sink->write(sink->user, k, strlen(k)) == GTEXT_MARKUP_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_WRITE;
  }
  case GTEXT_CSS_PROP_RESULT_VALID:
  case GTEXT_CSS_PROP_RESULT_PENDING: {
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(value->set);
    if (!root) {
      return GTEXT_CSS_OK; /* a value of nothing (an empty custom property) */
    }
    if (value->canonical && value->outcome == GTEXT_CSS_PROP_RESULT_VALID) {
      char * text = NULL;
      size_t length = 0;
      const GTEXT_CSS_Status st = serialize_canonical(value, options, root, &text, &length);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      const GTEXT_CSS_Status ws = length == 0 || sink->write(sink->user, text, length) == GTEXT_MARKUP_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_WRITE;
      gtext_css_serialize_buffer_free(options, text);
      return ws;
    }
    return gtext_css_serialize_value(root, options, sink);
  }
  default:
    return GTEXT_CSS_E_INVALID;
  }
}

GTEXT_CSS_Status gtext_css_property_value_serialize_buffer(const GTEXT_CSS_Property_Value * value,
    const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length) {
  if (!value || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  if (value->outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    const char * k = gtext_css_wide_keyword_name(value->wide);
    if (!k) {
      return GTEXT_CSS_E_INVALID;
    }
    GTEXT_Allocator a = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
    const size_t n = strlen(k);
    char * p = gtext_allocator_malloc(&a, n + 1);
    if (!p) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(p, k, n + 1);
    *out = p;
    *out_length = n;
    return GTEXT_CSS_OK;
  }
  if (value->outcome == GTEXT_CSS_PROP_RESULT_VALID || value->outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(value->set);
    if (root) {
      if (value->canonical && value->outcome == GTEXT_CSS_PROP_RESULT_VALID) {
        return serialize_canonical(value, options, root, out, out_length);
      }
      return gtext_css_serialize_value_buffer(root, options, out, out_length);
    }
    GTEXT_Allocator a = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
    char * p = gtext_allocator_malloc(&a, 1);
    if (!p) {
      return GTEXT_CSS_E_OOM;
    }
    p[0] = '\0';
    *out = p;
    *out_length = 0;
    return GTEXT_CSS_OK;
  }
  return GTEXT_CSS_E_INVALID;
}

/* ---- validity callbacks ------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_property_validity_init(GTEXT_CSS_Property_Validity * state, const GTEXT_CSS_Registry * registry,
    const GTEXT_CSS_Property_Options * options) {
  if (!state || !registry) {
    return GTEXT_CSS_E_INVALID;
  }
  memset(state, 0, sizeof *state);
  state->registry = registry;
  if (options) {
    state->options = *options;
  }
  state->options.at_rule = NULL;
  return GTEXT_CSS_OK;
}

static bool validity_declaration(void * user, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Declaration * declaration) {
  GTEXT_CSS_Property_Validity * v = user;
  GTEXT_CSS_Property_Options o = v->options;
  char at[PROP_NAME_STACK];
  if (parent && !parent->top_level && parent->kind == GTEXT_CSS_RULE_AT && parent->name.length + 2 <= sizeof at) {
    at[0] = '@';
    memcpy(at + 1, parent->name.data, parent->name.length);
    const GTEXT_CSS_At_Rule_Id rule = gtext_css_registry_find_at_rule(at, parent->name.length + 1);
    GTEXT_CSS_At_Rule_Info info;
    if (rule && gtext_css_registry_at_rule_info(rule, &info) && info.declarations) {
      at[parent->name.length + 1] = '\0';
      o.at_rule = at;
    }
  }
  GTEXT_CSS_Property_Value pv;
  const GTEXT_CSS_Status st = gtext_css_parse_property_declaration(v->registry, declaration, &o, &pv);
  v->checked++;
  bool valid = false;
  if (st == GTEXT_CSS_OK) {
    valid = pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE ||
            pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING;
  }
  else if (st == GTEXT_CSS_E_UNSUPPORTED) {
    v->undecided++;
    valid = true;
  }
  else if (v->status == GTEXT_CSS_OK) {
    v->status = st;
  }
  gtext_css_property_value_free(&pv);
  if (!valid) {
    v->rejected++;
  }
  return valid;
}

GTEXT_CSS_Validity gtext_css_property_validity(GTEXT_CSS_Property_Validity * state) {
  GTEXT_CSS_Validity v;
  memset(&v, 0, sizeof v);
  v.user = state;
  v.declaration = validity_declaration;
  return v;
}
