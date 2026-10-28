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
 * Presentation attributes (css_presentation.h): the table, the attribute-only spellings, and the entry in the cascade.
 */

#include <ghoti.io/text/css/css_presentation.h>

#include <stdint.h>
#include <string.h>

extern const GTEXT_CSS_Presentation_Table gtext_css_tbl_presentation;

const GTEXT_CSS_Presentation_Table * gtext_css_presentation_table_svg2(void) {
  return &gtext_css_tbl_presentation;
}

GTEXT_CSS_Presentation_Options gtext_css_presentation_options_default(void) {
  GTEXT_CSS_Presentation_Options o;
  memset(&o, 0, sizeof o);
  o.unitless_lengths = true;
  o.svg_transform = true;
  o.path_data = true;
  o.trim_whitespace = true;
  return o;
}

/* ---- the table ---------------------------------------------------------------------------------- */

/** Whether `name` is in the comma-separated list. */
static bool in_list(const char * list, const char * name, size_t length) {
  const char * p = list;
  while (*p) {
    const char * end = strchr(p, ',');
    const size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n == length && memcmp(p, name, n) == 0) {
      return true;
    }
    if (!end) {
      break;
    }
    p = end + 1;
  }
  return false;
}

bool css_presentation_applies(const GTEXT_CSS_Presentation_Row * row, const char * ns, size_t ns_length, const char * local, size_t local_length) {
  if (row->svg_namespace_only) {
    const size_t n = sizeof GTEXT_CSS_SVG_NAMESPACE - 1;
    if (!ns || ns_length != n || memcmp(ns, GTEXT_CSS_SVG_NAMESPACE, n) != 0) {
      return false;
    }
  }
  if (!row->elements) {
    return true;
  }
  if (row->elements[0] == '!') {
    return !in_list(row->elements + 1, local, local_length);
  }
  return in_list(row->elements, local, local_length);
}

const GTEXT_CSS_Presentation_Row * gtext_css_presentation_find(const GTEXT_CSS_Presentation_Table * table, const char * ns, size_t ns_length,
    const char * local_name, size_t local_name_length, const char * attribute, size_t attribute_length) {
  if (!table) {
    table = gtext_css_presentation_table_svg2();
  }
  if (!local_name || !attribute) {
    return NULL;
  }
  for (size_t i = 0; i < table->count; i++) {
    const GTEXT_CSS_Presentation_Row * row = &table->rows[i];
    if (strlen(row->attribute) == attribute_length && memcmp(row->attribute, attribute, attribute_length) == 0
        && css_presentation_applies(row, ns, ns_length, local_name, local_name_length)) {
      return row;
    }
  }
  return NULL;
}

/* ---- text building ------------------------------------------------------------------------------ */

typedef struct {
  const GTEXT_Allocator * alloc;
  char * data;
  size_t length, capacity;
  bool failed;
} text_buf;

static void tb_put(text_buf * b, const char * s, size_t n) {
  if (b->failed) {
    return;
  }
  if (b->length + n + 1 > b->capacity) {
    size_t cap = b->capacity ? b->capacity : 64;
    while (cap < b->length + n + 1) {
      if (cap > SIZE_MAX / 2) {
        b->failed = true;
        return;
      }
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(b->alloc, b->data, cap);
    if (!grown) {
      b->failed = true;
      return;
    }
    b->data = grown;
    b->capacity = cap;
  }
  memcpy(b->data + b->length, s, n);
  b->length += n;
  b->data[b->length] = '\0';
}

static void tb_str(text_buf * b, const char * s) {
  tb_put(b, s, strlen(s));
}

/* ---- white space -------------------------------------------------------------------------------- */

static bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* ---- the SVG transform list --------------------------------------------------------------------- */

typedef struct {
  const char * p;
  size_t n, i;
  size_t error; ///< The offset of the first thing wrong.
  bool bad;
} svg_in;

static void sv_ws(svg_in * in) {
  while (in->i < in->n && is_ws(in->p[in->i])) {
    in->i++;
  }
}

static bool sv_fail(svg_in * in) {
  if (!in->bad) {
    in->bad = true;
    in->error = in->i;
  }
  return false;
}

static bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

/** Scan a number (SVG 1.1 / 2: sign, digits with or without a point, exponent). Returns false with `bad` set when there is none. */
static bool sv_number(svg_in * in, size_t * start, size_t * end) {
  size_t i = in->i;
  const size_t s = i;
  if (i < in->n && (in->p[i] == '+' || in->p[i] == '-')) {
    i++;
  }
  size_t digits = 0;
  while (i < in->n && is_digit(in->p[i])) {
    i++;
    digits++;
  }
  if (i < in->n && in->p[i] == '.') {
    i++;
    size_t frac = 0;
    while (i < in->n && is_digit(in->p[i])) {
      i++;
      frac++;
    }
    digits += frac;
  }
  if (digits == 0) {
    in->i = s;
    return sv_fail(in);
  }
  if (i < in->n && (in->p[i] == 'e' || in->p[i] == 'E')) {
    size_t j = i + 1;
    if (j < in->n && (in->p[j] == '+' || in->p[j] == '-')) {
      j++;
    }
    if (j < in->n && is_digit(in->p[j])) {
      while (j < in->n && is_digit(in->p[j])) {
        j++;
      }
      i = j;
    }
  }
  *start = s;
  *end = i;
  in->i = i;
  return true;
}

/** Write a number as CSS reads it: `1.` and `1.e2` get the zero they lack; a number that is zero has no sign when it is negated. */
static void put_number(text_buf * b, const char * s, size_t n, bool negate) {
  size_t i = 0;
  bool neg = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    i++;
  }
  bool zero = true;
  for (size_t k = i; k < n && s[k] != 'e' && s[k] != 'E'; k++) {
    if (s[k] >= '1' && s[k] <= '9') {
      zero = false;
    }
  }
  if (negate) {
    neg = !neg;
  }
  if (neg && !(negate && zero)) {
    tb_put(b, "-", 1);
  }
  for (size_t k = i; k < n; k++) {
    tb_put(b, s + k, 1);
    if (s[k] == '.' && (k + 1 >= n || !is_digit(s[k + 1]))) {
      tb_put(b, "0", 1);
    }
  }
}

static void put_unit_number(text_buf * b, const char * s, size_t n, bool negate, const char * unit) {
  put_number(b, s, n, negate);
  tb_str(b, unit);
}

typedef struct {
  size_t s, e;
} span;

/**
 * Convert a transform list in SVG's syntax to CSS. Returns 1 converted, 0 not in the syntax (the offset in `*error`), -1 out of memory.
 */
static int svg_transform_to_css(const char * value, size_t length, text_buf * out, size_t * error) {
  svg_in in = {value, length, 0, 0, false};
  sv_ws(&in);
  if (in.i == in.n) {
    tb_str(out, "none");
    return out->failed ? -1 : 1;
  }
  bool first = true;
  for (;;) {
    size_t ns = in.i;
    while (in.i < in.n && ((value[in.i] >= 'a' && value[in.i] <= 'z') || (value[in.i] >= 'A' && value[in.i] <= 'Z'))) {
      in.i++;
    }
    const size_t nl = in.i - ns;
    int kind = -1; /* 0 matrix 1 translate 2 scale 3 rotate 4 skewX 5 skewY */
    static const struct {
      const char * name;
      size_t n;
    } names[] = {{"matrix", 6}, {"translate", 9}, {"scale", 5}, {"rotate", 6}, {"skewX", 5}, {"skewY", 5}};
    for (int k = 0; k < 6; k++) {
      if (names[k].n == nl && memcmp(names[k].name, value + ns, nl) == 0) {
        kind = k;
      }
    }
    if (kind < 0) {
      in.i = ns;
      sv_fail(&in);
      break;
    }
    sv_ws(&in);
    if (in.i >= in.n || value[in.i] != '(') {
      sv_fail(&in);
      break;
    }
    in.i++;
    sv_ws(&in);
    span args[6];
    size_t count = 0;
    bool ok = true;
    for (;;) {
      if (count == 6) {
        sv_fail(&in);
        ok = false;
        break;
      }
      if (!sv_number(&in, &args[count].s, &args[count].e)) {
        ok = false;
        break;
      }
      count++;
      sv_ws(&in);
      if (in.i < in.n && value[in.i] == ')') {
        break;
      }
      if (in.i < in.n && value[in.i] == ',') {
        in.i++;
        sv_ws(&in);
        if (in.i >= in.n || value[in.i] == ')') {
          sv_fail(&in); /* "comma-wsp number": a comma needs a number after it */
          ok = false;
          break;
        }
      }
      if (in.i >= in.n) {
        sv_fail(&in);
        ok = false;
        break;
      }
    }
    if (!ok) {
      break;
    }
    in.i++; /* ) */
    static const size_t lo[6] = {6, 1, 1, 1, 1, 1}, hi[6] = {6, 2, 2, 3, 1, 1};
    if (count < lo[kind] || count > hi[kind] || (kind == 3 && count == 2)) {
      in.i = ns;
      sv_fail(&in);
      break;
    }
    if (!first) {
      tb_str(out, " ");
    }
    first = false;
#define ARG(k) value + args[k].s, args[k].e - args[k].s
    switch (kind) {
    case 0:
      tb_str(out, "matrix(");
      for (size_t k = 0; k < 6; k++) {
        if (k) {
          tb_str(out, ", ");
        }
        put_number(out, ARG(k), false);
      }
      tb_str(out, ")");
      break;
    case 1:
      tb_str(out, "translate(");
      put_unit_number(out, ARG(0), false, "px");
      if (count == 2) {
        tb_str(out, ", ");
        put_unit_number(out, ARG(1), false, "px");
      }
      tb_str(out, ")");
      break;
    case 2:
      tb_str(out, "scale(");
      put_number(out, ARG(0), false);
      if (count == 2) {
        tb_str(out, ", ");
        put_number(out, ARG(1), false);
      }
      tb_str(out, ")");
      break;
    case 3:
      if (count == 3) {
        tb_str(out, "translate(");
        put_unit_number(out, ARG(1), false, "px");
        tb_str(out, ", ");
        put_unit_number(out, ARG(2), false, "px");
        tb_str(out, ") ");
      }
      tb_str(out, "rotate(");
      put_unit_number(out, ARG(0), false, "deg");
      tb_str(out, ")");
      if (count == 3) {
        tb_str(out, " translate(");
        put_unit_number(out, ARG(1), true, "px");
        tb_str(out, ", ");
        put_unit_number(out, ARG(2), true, "px");
        tb_str(out, ")");
      }
      break;
    case 4:
    case 5:
      tb_str(out, kind == 4 ? "skewX(" : "skewY(");
      put_unit_number(out, ARG(0), false, "deg");
      tb_str(out, ")");
      break;
    }
#undef ARG
    sv_ws(&in);
    if (in.i >= in.n) {
      return out->failed ? -1 : 1;
    }
    if (value[in.i] == ',') {
      in.i++;
      sv_ws(&in);
      if (in.i >= in.n) {
        sv_fail(&in);
        break;
      }
    }
  }
  *error = in.error;
  return 0;
}

/* ---- path data ---------------------------------------------------------------------------------- */

static void path_to_css(const char * value, size_t length, text_buf * out) {
  if (length == 0) {
    tb_str(out, "none");
    return;
  }
  tb_str(out, "\"");
  for (size_t i = 0; i < length; i++) {
    const char c = value[i];
    if (c == '\\' || c == '"') {
      tb_put(out, "\\", 1);
      tb_put(out, &c, 1);
    }
    else if (c == '\n') {
      tb_str(out, "\\a ");
    }
    else if (c == '\r') {
      tb_str(out, "\\d ");
    }
    else if (c == '\f') {
      tb_str(out, "\\c ");
    }
    else {
      tb_put(out, &c, 1);
    }
  }
  tb_str(out, "\"");
}

/* ---- the value ---------------------------------------------------------------------------------- */

typedef struct {
  const char * text; ///< The value to parse, in a buffer the caller frees (or the original when `owned` is false).
  size_t length;
  char * owned;
  size_t value_offset; ///< The trimmed value's place in the attribute value.
  size_t value_length;
  size_t error;        ///< Where the attribute-only syntax failed, or (size_t)-1.
} converted;

static GTEXT_CSS_Status convert(const GTEXT_CSS_Presentation_Row * row, const char * value, size_t length, const GTEXT_CSS_Presentation_Options * options,
    const GTEXT_Allocator * alloc, converted * c) {
  memset(c, 0, sizeof *c);
  c->error = (size_t)-1;
  size_t b = 0, e = length;
  if (options->trim_whitespace) {
    while (b < e && is_ws(value[b])) {
      b++;
    }
    while (e > b && is_ws(value[e - 1])) {
      e--;
    }
  }
  c->value_offset = b;
  c->value_length = e - b;
  c->text = value + b;
  c->length = e - b;
  if ((row->syntax == GTEXT_CSS_PRESENTATION_TRANSFORM && options->svg_transform) || (row->syntax == GTEXT_CSS_PRESENTATION_PATH && options->path_data)) {
    text_buf out = {alloc, NULL, 0, 0, false};
    if (row->syntax == GTEXT_CSS_PRESENTATION_PATH) {
      path_to_css(value + b, e - b, &out);
      if (out.failed) {
        gtext_allocator_free(alloc, out.data);
        return GTEXT_CSS_E_OOM;
      }
    }
    else {
      size_t error = 0;
      const int rc = svg_transform_to_css(value + b, e - b, &out, &error);
      if (rc < 0 || out.failed) {
        gtext_allocator_free(alloc, out.data);
        return GTEXT_CSS_E_OOM;
      }
      if (rc == 0) {
        /* not SVG's syntax: the property's own is tried, and the place SVG's failed is kept for the diagnostic */
        gtext_allocator_free(alloc, out.data);
        c->error = error;
        return GTEXT_CSS_OK;
      }
    }
    c->owned = out.data;
    c->text = out.data;
    c->length = out.length;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_presentation_value_text(const GTEXT_CSS_Presentation_Row * row, const char * value, size_t length,
    const GTEXT_CSS_Presentation_Options * options, const GTEXT_Allocator * allocator, char ** out, size_t * out_length, size_t * error_offset) {
  if (!row || !out || (!value && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Presentation_Options def = gtext_css_presentation_options_default();
  if (!options) {
    options = &def;
  }
  converted c;
  GTEXT_CSS_Status st = convert(row, value, length, options, allocator, &c);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (error_offset) {
    *error_offset = c.error == (size_t)-1 ? c.error : c.value_offset + c.error;
  }
  if (!c.owned) {
    char * copy = gtext_allocator_malloc(allocator, c.length + 1);
    if (!copy) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(copy, c.text, c.length);
    copy[c.length] = '\0';
    c.owned = copy;
  }
  *out = c.owned;
  if (out_length) {
    *out_length = c.length;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_candidates_add_presentation(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys,
    const GTEXT_CSS_Presentation_Row * row, const char * value, size_t length, const GTEXT_CSS_Presentation_Options * options,
    GTEXT_CSS_Presentation_Diagnostic * diagnostic, GTEXT_CSS_Cascade_Result * result) {
  GTEXT_CSS_Presentation_Diagnostic scratch;
  if (!diagnostic) {
    diagnostic = &scratch;
  }
  memset(diagnostic, 0, sizeof *diagnostic);
  if (!set || !keys || !row || (!value && length)) {
    if (result) {
      memset(result, 0, sizeof *result);
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
    }
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Presentation_Options def = gtext_css_presentation_options_default();
  if (!options) {
    options = &def;
  }
  const GTEXT_Allocator * alloc = NULL;
  converted c;
  GTEXT_CSS_Status st = convert(row, value, length, options, alloc, &c);
  if (st != GTEXT_CSS_OK) {
    if (result) {
      memset(result, 0, sizeof *result);
      result->status = st;
      result->message = gtext_css_status_string(st);
    }
    return st;
  }
  diagnostic->value_offset = options->base_offset + c.value_offset;
  diagnostic->value_length = c.value_length;
  GTEXT_CSS_Cascade_Entry k;
  memset(&k, 0, sizeof k);
  k.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  k.presentational_hint = true;
  k.context = keys->context;
  k.sheet = keys->sheet;
  k.rule = keys->rule;
  k.declaration = keys->declaration;
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.unitless_lengths = options->unitless_lengths;
  GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  st = gtext_css_candidates_add_text(set, &k, row->property, strlen(row->property), c.text, c.length, &po, &outcome, result);
  gtext_allocator_free(alloc, c.owned);
  if (st == GTEXT_CSS_E_UNSUPPORTED) {
    diagnostic->outcome = GTEXT_CSS_PRESENTATION_UNJUDGEABLE;
    diagnostic->offset = diagnostic->value_offset;
    return GTEXT_CSS_OK;
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN) {
    diagnostic->outcome = GTEXT_CSS_PRESENTATION_UNKNOWN_PROPERTY;
    diagnostic->offset = diagnostic->value_offset;
  }
  else if (outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
    diagnostic->outcome = GTEXT_CSS_PRESENTATION_INVALID_VALUE;
    diagnostic->offset = c.error == (size_t)-1 ? diagnostic->value_offset : diagnostic->value_offset + c.error;
  }
  return GTEXT_CSS_OK;
}
