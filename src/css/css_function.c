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
 * The function types as a resolver for the grammar engine, the families of function names, and the
 * helper that reads the leaves of a matched function value (the typed views of the family files
 * are built on it).
 *
 * The grammar of each type is in the family's file; this file holds the object that parses a definition
 * the first time it is asked for, keeps it, and hands it to a link, with a caller's own resolver (the
 * generated registry) consulted first.
 */

#include <string.h>

#include "css_function_internal.h"
#include "css_math_internal.h"

#define NFAMILIES 5

struct GTEXT_CSS_Function_Types {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Resolver fallback;
  void * fallback_user;
  GTEXT_CSS_Grammar_Options grammar;
  size_t total;
  GTEXT_CSS_Definition ** defs;  ///< One for each type, made when asked.
  unsigned char * state;         ///< 0 not made, 1 made, 2 the grammar of the type is wrong (a defect, cached).
  GTEXT_CSS_Status status;       ///< The last failure that was not a grammar error (out of memory), or OK.
};

typedef const css_fn_type * (*family_fn)(size_t *);

static const family_fn FAMILIES[NFAMILIES] = {css_image_types, css_transform_types, css_easing_types, css_shape_types, css_substitution_types};

size_t gtext_css_function_type_count(void) {
  size_t total = 0;
  for (size_t f = 0; f < NFAMILIES; f++) {
    size_t n;
    FAMILIES[f](&n);
    total += n;
  }
  return total;
}

static const css_fn_type * type_at(size_t index) {
  for (size_t f = 0; f < NFAMILIES; f++) {
    size_t n;
    const css_fn_type * t = FAMILIES[f](&n);
    if (index < n) {
      return &t[index];
    }
    index -= n;
  }
  return NULL;
}

bool gtext_css_function_type_at(size_t index, const char ** name, GTEXT_CSS_Ref_Kind * kind, const char ** grammar) {
  const css_fn_type * t = type_at(index);
  if (!t) {
    return false;
  }
  if (name) {
    *name = t->name;
  }
  if (kind) {
    *kind = t->kind;
  }
  if (grammar) {
    *grammar = t->grammar;
  }
  return true;
}

GTEXT_CSS_Status gtext_css_function_types_new(const GTEXT_CSS_Function_Types_Options * options, GTEXT_CSS_Function_Types ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_Allocator alloc = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
  GTEXT_CSS_Function_Types * t = gtext_allocator_calloc(&alloc, 1, sizeof *t);
  if (!t) {
    return GTEXT_CSS_E_OOM;
  }
  t->alloc = alloc;
  t->total = gtext_css_function_type_count();
  t->defs = gtext_allocator_calloc(&alloc, t->total, sizeof *t->defs);
  t->state = gtext_allocator_calloc(&alloc, t->total, 1);
  if (!t->defs || !t->state) {
    gtext_allocator_free(&alloc, t->defs);
    gtext_allocator_free(&alloc, t->state);
    gtext_allocator_free(&alloc, t);
    return GTEXT_CSS_E_OOM;
  }
  t->fallback = options ? options->fallback : NULL;
  t->fallback_user = options ? options->fallback_user : NULL;
  t->grammar = options && options->grammar ? *options->grammar : gtext_css_grammar_options_default();
  t->grammar.allocator = NULL; /* the definitions are made through the types' own allocator below */
  *out = t;
  return GTEXT_CSS_OK;
}

void gtext_css_function_types_free(GTEXT_CSS_Function_Types * t) {
  if (!t) {
    return;
  }
  for (size_t i = 0; i < t->total; i++) {
    gtext_css_definition_free(t->defs[i]);
  }
  GTEXT_Allocator alloc = t->alloc;
  gtext_allocator_free(&alloc, t->defs);
  gtext_allocator_free(&alloc, t->state);
  gtext_allocator_free(&alloc, t);
}

const GTEXT_CSS_Definition * gtext_css_function_types_resolve(void * user, GTEXT_CSS_Ref_Kind kind, const char * name, size_t length) {
  GTEXT_CSS_Function_Types * t = user;
  if (!t || !name) {
    return NULL;
  }
  t->status = GTEXT_CSS_OK;
  if (t->fallback) {
    const GTEXT_CSS_Definition * d = t->fallback(t->fallback_user, kind, name, length);
    if (d) {
      return d;
    }
  }
  size_t index = 0;
  for (size_t f = 0; f < NFAMILIES; f++) {
    size_t n;
    const css_fn_type * tab = FAMILIES[f](&n);
    for (size_t i = 0; i < n; i++, index++) {
      if (tab[i].kind == kind && strlen(tab[i].name) == length && memcmp(tab[i].name, name, length) == 0) {
        if (t->state[index] == 0) {
          GTEXT_CSS_Grammar_Options go = t->grammar;
          go.allocator = &t->alloc;
          GTEXT_CSS_Definition * d = NULL;
          const GTEXT_CSS_Status st = gtext_css_definition_parse(tab[i].grammar, strlen(tab[i].grammar), &go, &d, NULL);
          if (st == GTEXT_CSS_OK) {
            t->defs[index] = d;
            t->state[index] = 1;
          }
          else if (st == GTEXT_CSS_E_OOM || st == GTEXT_CSS_E_LIMIT_DEFINITION || st == GTEXT_CSS_E_LIMIT_DEFINITION_NODES ||
                   st == GTEXT_CSS_E_LIMIT_DEFINITION_DEPTH) {
            t->status = st; /* not a defect of the grammar: nothing is cached, and the next ask tries again */
          }
          else {
            t->state[index] = 2;
          }
        }
        return t->state[index] == 1 ? t->defs[index] : NULL;
      }
    }
  }
  return NULL;
}

GTEXT_CSS_Status gtext_css_function_types_status(const GTEXT_CSS_Function_Types * t) {
  return t ? t->status : GTEXT_CSS_E_INVALID;
}

GTEXT_CSS_Status gtext_css_function_types_prepare(GTEXT_CSS_Function_Types * t) {
  if (!t) {
    return GTEXT_CSS_E_INVALID;
  }
  t->status = GTEXT_CSS_OK;
  for (size_t f = 0; f < NFAMILIES; f++) {
    size_t n;
    const css_fn_type * tab = FAMILIES[f](&n);
    for (size_t i = 0; i < n; i++) {
      (void)gtext_css_function_types_resolve(t, tab[i].kind, tab[i].name, strlen(tab[i].name));
      if (t->status != GTEXT_CSS_OK) {
        return t->status;
      }
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- families ------------------------------------------------------------------ */

static char fold1(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

bool css_fn_name_is(const GTEXT_CSS_String * s, const char * lower) {
  const size_t n = strlen(lower);
  if (s->length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (fold1(s->data[i]) != lower[i]) {
      return false;
    }
  }
  return true;
}

static bool in_list(const GTEXT_CSS_String * s, const char * const * names, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (css_fn_name_is(s, names[i])) {
      return true;
    }
  }
  return false;
}

GTEXT_CSS_Function_Family gtext_css_function_family(const char * name, size_t length) {
  if (!name) {
    return GTEXT_CSS_FN_FAMILY_NONE;
  }
  const GTEXT_CSS_String s = {name, length};
  static const char * const image[] = {"url", "src", "image", "image-set", "-webkit-image-set", "cross-fade", "element", "paint"};
  static const char * const gradient[] = {"linear-gradient", "repeating-linear-gradient", "radial-gradient", "repeating-radial-gradient",
    "conic-gradient", "repeating-conic-gradient"};
  static const char * const transform[] = {"matrix", "translate", "translatex", "translatey", "translatez", "translate3d", "scale", "scalex",
    "scaley", "scalez", "scale3d", "rotate", "rotatex", "rotatey", "rotatez", "rotate3d", "skew", "skewx", "skewy", "matrix3d", "perspective"};
  static const char * const easing[] = {"linear", "cubic-bezier", "steps"};
  static const char * const shape[] = {"inset", "xywh", "rect", "circle", "ellipse", "polygon", "path", "shape"};
  static const char * const subst[] = {"var", "env", "attr", "if", "first-valid", "random", "random-item", "sibling-index", "sibling-count",
    "progress", "inherit", "ident", "string", "counter", "counters", "symbols", "calc-size"};
  static const char * const color[] = {"rgb", "rgba", "hsl", "hsla", "hwb", "lab", "lch", "oklab", "oklch", "color", "color-mix", "light-dark",
    "contrast-color", "alpha", "device-cmyk"};
  if (gtext_css_math_is_function(name, length)) {
    return GTEXT_CSS_FN_FAMILY_MATH;
  }
  if (in_list(&s, color, sizeof color / sizeof color[0])) {
    return GTEXT_CSS_FN_FAMILY_COLOR;
  }
  if (in_list(&s, gradient, sizeof gradient / sizeof gradient[0])) {
    return GTEXT_CSS_FN_FAMILY_GRADIENT;
  }
  if (in_list(&s, image, sizeof image / sizeof image[0])) {
    return GTEXT_CSS_FN_FAMILY_IMAGE;
  }
  if (in_list(&s, transform, sizeof transform / sizeof transform[0])) {
    return GTEXT_CSS_FN_FAMILY_TRANSFORM;
  }
  if (in_list(&s, easing, sizeof easing / sizeof easing[0])) {
    return GTEXT_CSS_FN_FAMILY_EASING;
  }
  if (in_list(&s, shape, sizeof shape / sizeof shape[0])) {
    return GTEXT_CSS_FN_FAMILY_SHAPE;
  }
  if (in_list(&s, subst, sizeof subst / sizeof subst[0])) {
    return GTEXT_CSS_FN_FAMILY_SUBSTITUTION;
  }
  return GTEXT_CSS_FN_FAMILY_NONE;
}

/* ---- the leaves of a matched function --------------------------------------------- */

static bool leafvec_push(const GTEXT_Allocator * alloc, css_leafvec * out, const GTEXT_CSS_Value * v) {
  if (out->n == out->cap) {
    const size_t cap = out->cap ? out->cap * 2 : 16;
    const GTEXT_CSS_Value ** p = gtext_allocator_realloc(alloc, out->v, cap * sizeof *p);
    if (!p) {
      return false;
    }
    out->v = p;
    out->cap = cap;
  }
  out->v[out->n++] = v;
  return true;
}

void css_fn_leaves_free(const GTEXT_Allocator * alloc, css_leafvec * out) {
  gtext_allocator_free(alloc, out->v);
  out->v = NULL;
  out->n = out->cap = 0;
}

typedef struct {
  const GTEXT_CSS_Value * v;
  size_t i;
} lframe;

bool css_fn_leaves(const GTEXT_Allocator * alloc, const GTEXT_CSS_Value * fn, css_leafvec * out) {
  out->v = NULL;
  out->n = out->cap = 0;
  size_t cap = 16, ns = 0;
  lframe * st = gtext_allocator_malloc(alloc, cap * sizeof *st);
  if (!st) {
    return false;
  }
  st[ns].v = fn;
  st[ns].i = 0;
  ns++;
  bool ok = true;
  while (ok && ns > 0) {
    lframe * f = &st[ns - 1];
    if (f->i >= f->v->child_count) {
      ns--;
      continue;
    }
    const GTEXT_CSS_Value * parent = f->v;
    const GTEXT_CSS_Value * c = &parent->children[f->i];
    /* a comma between the items of a comma-separated list */
    if (f->i > 0 && parent->kind == GTEXT_CSS_VAL_LIST && parent->separator == GTEXT_CSS_SEP_COMMA) {
      ok = leafvec_push(alloc, out, NULL);
    }
    f->i++;
    if (!ok) {
      break;
    }
    if (c->kind == GTEXT_CSS_VAL_TOKEN && c->text.length == 1 && c->text.data[0] == ',') {
      ok = leafvec_push(alloc, out, NULL);
    }
    else if ((c->kind == GTEXT_CSS_VAL_LIST || c->kind == GTEXT_CSS_VAL_BLOCK) && c->child_count > 0) {
      if (ns == cap) {
        lframe * p = gtext_allocator_realloc(alloc, st, cap * 2 * sizeof *p);
        if (!p) {
          ok = false;
          break;
        }
        st = p;
        cap *= 2;
      }
      st[ns].v = c;
      st[ns].i = 0;
      ns++;
    }
    else {
      ok = leafvec_push(alloc, out, c);
    }
  }
  gtext_allocator_free(alloc, st);
  if (!ok) {
    css_fn_leaves_free(alloc, out);
  }
  return ok;
}

int css_fn_leaf_number(const GTEXT_CSS_Value * v, double * value, GTEXT_CSS_Unit * unit) {
  if (unit) {
    *unit = GTEXT_CSS_UNIT_NONE;
  }
  if (!v) {
    return -1;
  }
  switch (v->kind) {
  case GTEXT_CSS_VAL_NUMBER: case GTEXT_CSS_VAL_INTEGER:
    *value = v->number;
    return 0;
  case GTEXT_CSS_VAL_ZERO:
    *value = 0.0;
    return 0;
  case GTEXT_CSS_VAL_PERCENTAGE:
    *value = v->number;
    return 1;
  case GTEXT_CSS_VAL_DIMENSION: {
    double c = v->number;
    switch (gtext_css_unit_type(v->unit)) {
    case GTEXT_CSS_UNIT_TYPE_LENGTH:
      *value = v->number;
      if (unit) {
        *unit = v->unit;
      }
      return 2;
    case GTEXT_CSS_UNIT_TYPE_ANGLE:
      (void)gtext_css_unit_to_canonical(v->unit, v->number, &c);
      *value = c;
      return 3;
    case GTEXT_CSS_UNIT_TYPE_TIME:
      (void)gtext_css_unit_to_canonical(v->unit, v->number, &c);
      *value = c;
      return 4;
    case GTEXT_CSS_UNIT_TYPE_RESOLUTION:
      (void)gtext_css_unit_to_canonical(v->unit, v->number, &c);
      *value = c;
      return 5;
    default:
      return -1;
    }
  }
  case GTEXT_CSS_VAL_FUNCTION:
    if (v->math) {
      double x;
      GTEXT_CSS_Unit u;
      bool pct;
      if (!gtext_css_math_numeric(v->math, &x, &u, &pct)) {
        return -1;
      }
      *value = x;
      if (pct) {
        return 1;
      }
      switch (v->math->category) {
      case GTEXT_CSS_MATH_CAT_NUMBER: return 0;
      case GTEXT_CSS_MATH_CAT_LENGTH:
        if (unit) {
          *unit = u;
        }
        return 2;
      case GTEXT_CSS_MATH_CAT_ANGLE: return 3;
      case GTEXT_CSS_MATH_CAT_TIME: return 4;
      case GTEXT_CSS_MATH_CAT_RESOLUTION: return 5;
      default: return -1;
      }
    }
    return -1;
  default:
    return -1;
  }
}

bool css_fn_leaf_is_keyword(const GTEXT_CSS_Value * v, const char * lower) {
  return v && v->kind == GTEXT_CSS_VAL_KEYWORD && css_fn_name_is(&v->text, lower);
}
