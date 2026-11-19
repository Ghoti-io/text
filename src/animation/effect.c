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
 * Keyframe effects (animation/effect.h): the keyframes and the processing of a `keyframes` argument, the `KeyframeEffect` object, and what the sampler reads of it. Each static function that is a
 * procedure of the specification names its section.
 */

#include <ghoti.io/text/animation/effect.h>

#include <string.h>

#include <ghoti.io/text/css/css_cssom_decl.h>

#include "animation_internal.h"
#include "world_internal.h"

/* ---- keyframes ----------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * name;                          ///< The animation property name as a CSS property (`margin-left`, a custom property's name).
  size_t name_length;
  char * idl;                           ///< Its IDL attribute name (`marginLeft`, `cssFloat`).
  size_t idl_length;
  char * input;                         ///< The name the object had it under (the sort key of the properties of a keyframe-like object).
  size_t input_length;
  char * value;                         ///< The value: as it was read, then (after the parse) serialized.
  size_t value_length;
} kprop;

typedef struct {
  bool offset_null;
  double offset;
  double computed;                      ///< The computed keyframe offset.
  GTEXT_CSS_Easing * easing;            ///< Parsed; NULL is `linear` until the keyframes are final.
  char * easing_text;                   ///< The text as given, then serialized (NULL: `linear`).
  size_t easing_length;
  GTEXT_Anim_Keyframe_Composite composite;
  kprop * props;
  size_t nprops;
} kframe;

struct GTEXT_Anim_Keyframes {
  GTEXT_Allocator alloc;
  kframe * frames;
  size_t n;
  size_t cap;
};

static char * dup_bytes(const GTEXT_Allocator * a, const char * s, size_t n) {
  char * p = gtext_allocator_malloc(a, n + 1);
  if (!p) {
    return NULL;
  }
  if (n) {
    memcpy(p, s, n);
  }
  p[n] = '\0';
  return p;
}

static void kprop_free(const GTEXT_Allocator * a, kprop * p) {
  gtext_allocator_free(a, p->name);
  gtext_allocator_free(a, p->idl);
  gtext_allocator_free(a, p->input);
  gtext_allocator_free(a, p->value);
}

static void kframe_free(const GTEXT_Allocator * a, kframe * f) {
  for (size_t i = 0; i < f->nprops; i++) {
    kprop_free(a, &f->props[i]);
  }
  gtext_allocator_free(a, f->props);
  gtext_css_easing_free(f->easing);
  gtext_allocator_free(a, f->easing_text);
  memset(f, 0, sizeof *f);
}

static GTEXT_Anim_Keyframes * kf_new(const GTEXT_Allocator * a) {
  GTEXT_Anim_Keyframes * k = gtext_allocator_calloc(a, 1, sizeof *k);
  if (k) {
    k->alloc = *a;
  }
  return k;
}

void gtext_anim_keyframes_free(GTEXT_Anim_Keyframes * k) {
  if (!k) {
    return;
  }
  GTEXT_Allocator a = k->alloc;
  for (size_t i = 0; i < k->n; i++) {
    kframe_free(&a, &k->frames[i]);
  }
  gtext_allocator_free(&a, k->frames);
  gtext_allocator_free(&a, k);
}

/// Room for one more keyframe; the new one is zeroed and counted.
static kframe * kf_push(GTEXT_Anim_Keyframes * k) {
  if (k->n == k->cap) {
    const size_t cap = k->cap ? k->cap * 2 : 4;
    kframe * bigger = gtext_allocator_realloc(&k->alloc, k->frames, cap * sizeof *bigger);
    if (!bigger) {
      return NULL;
    }
    k->frames = bigger;
    k->cap = cap;
  }
  kframe * f = &k->frames[k->n++];
  memset(f, 0, sizeof *f);
  f->offset_null = true;
  return f;
}

static bool kframe_add_prop(const GTEXT_Allocator * a, kframe * f, const kprop * p) {
  kprop * bigger = gtext_allocator_realloc(a, f->props, (f->nprops + 1) * sizeof *bigger);
  if (!bigger) {
    return false;
  }
  f->props = bigger;
  f->props[f->nprops++] = *p;
  return true;
}

size_t gtext_anim_keyframes_count(const GTEXT_Anim_Keyframes * k) {
  return k ? k->n : 0;
}

bool gtext_anim_keyframes_at(const GTEXT_Anim_Keyframes * k, size_t index, GTEXT_Anim_Keyframe_Info * out) {
  if (!k || !out || index >= k->n) {
    return false;
  }
  const kframe * f = &k->frames[index];
  out->offset_null = f->offset_null;
  out->offset = f->offset_null ? 0.0 : f->offset;
  out->computed_offset = f->computed;
  out->easing.data = f->easing_text ? f->easing_text : "linear";
  out->easing.length = f->easing_text ? f->easing_length : 6;
  out->composite = f->composite;
  out->property_count = f->nprops;
  return true;
}

bool gtext_anim_keyframes_property_at(const GTEXT_Anim_Keyframes * k, size_t index, size_t property, GTEXT_CSS_String * name, GTEXT_CSS_String * idl_name, GTEXT_CSS_String * value) {
  if (!k || index >= k->n || property >= k->frames[index].nprops) {
    return false;
  }
  const kprop * p = &k->frames[index].props[property];
  if (name) {
    *name = (GTEXT_CSS_String){p->name, p->name_length};
  }
  if (idl_name) {
    *idl_name = (GTEXT_CSS_String){p->idl, p->idl_length};
  }
  if (value) {
    *value = (GTEXT_CSS_String){p->value, p->value_length};
  }
  return true;
}

/* ---- property names --------------------------------------------------------------------------------------------------------------------------- */

static bool text_is(const GTEXT_Anim_Text * t, const char * lit) {
  const size_t n = strlen(lit);
  return t->length == n && (n == 0 || memcmp(t->data, lit, n) == 0);
}

static bool is_custom(const char * s, size_t n) {
  return n > 2 && s[0] == '-' && s[1] == '-';
}

/// Whether the property can be animated: it is a longhand whose animation type is not "not animatable", or a shorthand with such a longhand ("a list of property names (including shorthand properties that have
/// longhand sub-properties that are animatable) that can be animated by the UA").
static bool animatable_property(const GTEXT_CSS_Registry * reg, GTEXT_CSS_Property_Id id) {
  GTEXT_CSS_Property_Info pi;
  if (!gtext_css_registry_property_info(reg, id, &pi)) {
    return false;
  }
  if (!(pi.flags & GTEXT_CSS_PROP_SHORTHAND)) {
    return pi.animation != GTEXT_CSS_ANIM_NOT_ANIMATABLE;
  }
  for (size_t i = 0;; i++) {
    const GTEXT_CSS_Property_Id lh = gtext_css_registry_longhand(reg, id, i);
    if (!lh) {
      return false;
    }
    if (animatable_property(reg, lh)) {
      return true;
    }
  }
}

/// "IDL attribute name to animation property name": the CSS property an attribute stands for. `*name` is a new string; `*skip` is set for a name that is not an animatable property (the property is then not a
/// property of the keyframe: "a list of all of the properties that are in both input properties and animatable properties, or ... custom-property-name").
static GTEXT_Anim_Status property_from_idl(const GTEXT_Anim_World * w, const GTEXT_Anim_Text * attr, char ** name, size_t * name_length, bool * skip) {
  *name = NULL;
  *name_length = 0;
  *skip = true;
  if (is_custom(attr->data, attr->length)) {
    *name = dup_bytes(&w->alloc, attr->data, attr->length);
    *name_length = attr->length;
    *skip = false;
    return *name ? GTEXT_ANIM_OK : GTEXT_ANIM_OOM;
  }
  const char * prop = NULL;
  size_t plen = 0;
  if (text_is(attr, "cssFloat")) {
    prop = "float";
    plen = 5;
  }
  else if (text_is(attr, "cssOffset")) {
    prop = "offset";
    plen = 6;
  }
  else if (text_is(attr, "float") || text_is(attr, "offset") || !gtext_cssom_idl_property(attr->data, attr->length, &prop, &plen)) {
    return GTEXT_ANIM_OK;   /* `float` and `offset` are not IDL attributes of animation properties: `cssFloat` and `cssOffset` are */
  }
  const GTEXT_CSS_Registry * reg = anim_world_registry(w);
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(reg, prop, plen);
  if (!id || !animatable_property(reg, id)) {
    return GTEXT_ANIM_OK;
  }
  *name = dup_bytes(&w->alloc, prop, plen);
  *name_length = plen;
  *skip = false;
  return *name ? GTEXT_ANIM_OK : GTEXT_ANIM_OOM;
}

/// "Animation property name to IDL attribute name".
static GTEXT_Anim_Status idl_from_property(const GTEXT_Anim_World * w, const char * name, size_t length, char ** idl, size_t * idl_length) {
  if (is_custom(name, length)) {
    *idl = dup_bytes(&w->alloc, name, length);
  }
  else if (length == 5 && memcmp(name, "float", 5) == 0) {
    *idl = dup_bytes(&w->alloc, "cssFloat", 8);
    length = 8;
  }
  else if (length == 6 && memcmp(name, "offset", 6) == 0) {
    *idl = dup_bytes(&w->alloc, "cssOffset", 9);
    length = 9;
  }
  else {
    const GTEXT_CSS_Status st = gtext_cssom_idl_attribute(name, length, &w->alloc, idl, &length);
    if (st != GTEXT_CSS_OK) {
      *idl = NULL;
      return st == GTEXT_CSS_E_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
    }
  }
  *idl_length = length;
  return *idl ? GTEXT_ANIM_OK : GTEXT_ANIM_OOM;
}

/* ---- processing a keyframes argument ----------------------------------------------------------------------------------------------------------- */

/// A property of a keyframe-like object, once filtered: it holds the name the object had it under (the sort key) and the pair.
static GTEXT_Anim_Status make_prop(const GTEXT_Anim_World * w, const GTEXT_Anim_Text * input_name, const GTEXT_Anim_Text * value, kprop * out, bool * skip) {
  memset(out, 0, sizeof *out);
  GTEXT_Anim_Status st = property_from_idl(w, input_name, &out->name, &out->name_length, skip);
  if (st != GTEXT_ANIM_OK || *skip) {
    return st;
  }
  st = idl_from_property(w, out->name, out->name_length, &out->idl, &out->idl_length);
  if (st == GTEXT_ANIM_OK) {
    out->input = dup_bytes(&w->alloc, input_name->data, input_name->length);
    out->input_length = input_name->length;
    out->value = dup_bytes(&w->alloc, value->data ? value->data : "", value->length);
    out->value_length = value->length;
    if (!out->input || !out->value) {
      st = GTEXT_ANIM_OOM;
    }
  }
  if (st != GTEXT_ANIM_OK) {
    kprop_free(&w->alloc, out);
    memset(out, 0, sizeof *out);
  }
  return st;
}

static int cmp_bytes(const char * a, size_t an, const char * b, size_t bn) {
  const size_t m = an < bn ? an : bn;
  const int c = m ? memcmp(a, b, m) : 0;
  return c ? c : (an < bn ? -1 : (an > bn ? 1 : 0));
}

/// "Compute missing keyframe offsets": the computed keyframe offsets of a list of keyframes.
static void compute_missing_offsets(kframe * frames, size_t n) {
  for (size_t i = 0; i < n; i++) {
    frames[i].computed = frames[i].offset_null ? 0.0 : frames[i].offset;
  }
  if (!n) {
    return;
  }
  bool first_set = !frames[0].offset_null, last_set = !frames[n - 1].offset_null;
  if (n > 1 && !first_set) {
    frames[0].computed = 0.0;
    first_set = true;
  }
  if (!last_set) {
    frames[n - 1].computed = 1.0;
    last_set = true;
  }
  /* each run of null offsets between two keyframes that have one */
  size_t a = 0;
  bool a_set = first_set && (n > 1 || !frames[0].offset_null);
  if (n == 1) {
    return;
  }
  for (size_t b = 1; b < n; b++) {
    const bool b_set = !frames[b].offset_null || b == n - 1;
    if (!b_set) {
      continue;
    }
    if (a_set) {
      const double oa = frames[a].computed, ob = frames[b].computed;
      const size_t m = b - a;
      for (size_t k = a + 1; k < b; k++) {
        frames[k].computed = oa + (ob - oa) * (double)(k - a) / (double)m;
      }
    }
    a = b;
    a_set = true;
  }
}

/// Parse an easing text for a keyframe ("parsing the easing property ... using the CSS syntax defined for the easing member of EffectTiming": a failure is a TypeError).
static GTEXT_Anim_Status keyframe_easing(const GTEXT_Anim_World * w, const GTEXT_Anim_Text * text, GTEXT_CSS_Easing ** out, char ** serialized, size_t * serialized_length) {
  GTEXT_CSS_Easing * e = NULL;
  const GTEXT_Anim_Status st = gtext_anim_easing_parse(anim_world_registry(w), text->data ? text->data : "linear", text->data ? text->length : 6, &w->alloc, &e);
  if (st != GTEXT_ANIM_OK) {
    return st == GTEXT_ANIM_INVALID ? GTEXT_ANIM_TYPE_ERROR : st;
  }
  if (out) {
    char * text_out = NULL;
    size_t n = 0;
    const GTEXT_CSS_Status ss = gtext_css_easing_serialize_buffer(e, &w->alloc, &text_out, &n);
    if (ss != GTEXT_CSS_OK) {
      gtext_css_easing_free(e);
      return ss == GTEXT_CSS_E_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
    }
    *out = e;
    *serialized = text_out;
    *serialized_length = n;
  }
  else {
    gtext_css_easing_free(e);
  }
  return GTEXT_ANIM_OK;
}

/// "For each property-value pair in frame, parse the property value using the syntax specified for that property. If the property value is invalid according to the syntax for the property, discard the
/// property-value pair." The value of a pair that stays is serialized as CSSOM serializes a specified value.
static GTEXT_Anim_Status finish_props(const GTEXT_Anim_World * w, kframe * f) {
  const GTEXT_CSS_Registry * reg = anim_world_registry(w);
  size_t kept = 0;
  GTEXT_Anim_Status result = GTEXT_ANIM_OK;
  for (size_t i = 0; i < f->nprops; i++) {
    kprop * p = &f->props[i];
    GTEXT_CSS_Property_Value pv;
    GTEXT_CSS_Property_Options po;
    memset(&po, 0, sizeof po);
    po.allocator = &w->alloc;
    const GTEXT_CSS_Status ps = gtext_css_parse_property_text(reg, p->name, p->name_length, p->value, p->value_length, &po, &pv);
    bool keep = false;
    if (ps == GTEXT_CSS_E_OOM) {
      result = GTEXT_ANIM_OOM;
    }
    else if (ps == GTEXT_CSS_E_UNSUPPORTED) {
      keep = true;   /* a property the engine cannot judge: its text stands */
    }
    else if (ps == GTEXT_CSS_OK && (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING)) {
      char * text = NULL;
      size_t n = 0;
      GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
      so.allocator = &w->alloc;
      const GTEXT_CSS_Status ss = gtext_css_property_value_serialize_buffer(&pv, &so, &text, &n);
      if (ss == GTEXT_CSS_OK) {
        char * copy = dup_bytes(&w->alloc, text, n);
        gtext_css_serialize_buffer_free(&so, text);
        if (copy) {
          gtext_allocator_free(&w->alloc, p->value);
          p->value = copy;
          p->value_length = n;
          keep = true;
        }
        else {
          result = GTEXT_ANIM_OOM;
        }
      }
      else if (ss == GTEXT_CSS_E_OOM) {
        result = GTEXT_ANIM_OOM;
      }
      else {
        keep = true;
      }
    }
    gtext_css_property_value_free(&pv);
    if (result != GTEXT_ANIM_OK) {
      return result;
    }
    if (keep) {
      f->props[kept++] = *p;
    }
    else {
      kprop_free(&w->alloc, p);
    }
  }
  f->nprops = kept;
  return GTEXT_ANIM_OK;
}

static bool finite_offset(const GTEXT_Anim_Offset * o) {
  return o->is_null || (o->value == o->value && o->value - o->value == 0.0);
}

/// The steps that follow the assembly of "processed keyframes" in both forms.
static GTEXT_Anim_Status finish_keyframes(GTEXT_Anim_World * w, GTEXT_Anim_Keyframes * k, const GTEXT_Anim_Text * unused_easings, size_t n_unused) {
  /* "If processed keyframes is not loosely sorted by offset, throw a TypeError" */
  double previous = 0.0;
  bool have_previous = false;
  for (size_t i = 0; i < k->n; i++) {
    if (k->frames[i].offset_null) {
      continue;
    }
    if (have_previous && k->frames[i].offset < previous) {
      return GTEXT_ANIM_TYPE_ERROR;
    }
    previous = k->frames[i].offset;
    have_previous = true;
  }
  /* "If there exist any keyframe in processed keyframes whose keyframe offset is non-null and less than zero or greater than one, throw a TypeError" */
  for (size_t i = 0; i < k->n; i++) {
    if (!k->frames[i].offset_null && (k->frames[i].offset < 0.0 || k->frames[i].offset > 1.0)) {
      return GTEXT_ANIM_TYPE_ERROR;
    }
  }
  for (size_t i = 0; i < k->n; i++) {
    GTEXT_Anim_Status st = finish_props(w, &k->frames[i]);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
  }
  /* "Let the easing function of frame be the result of parsing the easing property": a failure is a TypeError, after every property was read */
  for (size_t i = 0; i < k->n; i++) {
    kframe * f = &k->frames[i];
    GTEXT_Anim_Text t = {f->easing_text, f->easing_length};
    GTEXT_CSS_Easing * e = NULL;
    char * ser = NULL;
    size_t sl = 0;
    const GTEXT_Anim_Status st = keyframe_easing(w, &t, &e, &ser, &sl);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
    gtext_allocator_free(&w->alloc, f->easing_text);
    f->easing = e;
    f->easing_text = ser;
    f->easing_length = sl;
  }
  /* "Parse each of the values in unused easings ... if any of the values fail to parse, throw a TypeError" */
  for (size_t i = 0; i < n_unused; i++) {
    const GTEXT_Anim_Status st = keyframe_easing(w, &unused_easings[i], NULL, NULL, NULL);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
  }
  compute_missing_offsets(k->frames, k->n);
  return GTEXT_ANIM_OK;
}

/// Set the easing text of a frame to the text the binding read (parsed at the end), or "linear".
static GTEXT_Anim_Status set_easing_text(const GTEXT_Anim_World * w, kframe * f, const GTEXT_Anim_Text * t) {
  gtext_allocator_free(&w->alloc, f->easing_text);
  f->easing_text = NULL;
  f->easing_length = 0;
  if (t->data) {
    f->easing_text = dup_bytes(&w->alloc, t->data, t->length);
    f->easing_length = t->length;
    return f->easing_text ? GTEXT_ANIM_OK : GTEXT_ANIM_OOM;
  }
  return GTEXT_ANIM_OK;
}

/// Insert `p` into the properties of a frame, in the order of the code points of the names the object had them under ("Sort animation properties in ascending order").
static bool insert_sorted(const GTEXT_Allocator * a, kframe * f, const kprop * p) {
  if (!kframe_add_prop(a, f, p)) {
    return false;
  }
  size_t i = f->nprops - 1;
  while (i > 0 && cmp_bytes(f->props[i - 1].input, f->props[i - 1].input_length, f->props[i].input, f->props[i].input_length) > 0) {
    const kprop t = f->props[i - 1];
    f->props[i - 1] = f->props[i];
    f->props[i] = t;
    i--;
  }
  return true;
}

static GTEXT_Anim_Status process_array(GTEXT_Anim_World * w, const GTEXT_Anim_Keyframes_Argument * arg, GTEXT_Anim_Keyframes * k) {
  for (size_t i = 0; i < arg->array_count; i++) {
    const GTEXT_Anim_Keyframe_Like * in = &arg->array[i];
    if (!finite_offset(&in->offset)) {
      return GTEXT_ANIM_TYPE_ERROR;
    }
    kframe * f = kf_push(k);
    if (!f) {
      return GTEXT_ANIM_OOM;
    }
    if (in->null_entry) {
      continue;   /* a null or undefined element: a keyframe-like object with no members, the defaults */
    }
    f->offset_null = in->offset.is_null;
    f->offset = in->offset.is_null ? 0.0 : in->offset.value;
    f->composite = in->composite;
    GTEXT_Anim_Status st = set_easing_text(w, f, &in->easing);
    for (size_t j = 0; j < in->property_count && st == GTEXT_ANIM_OK; j++) {
      const GTEXT_Anim_Text * name = &in->properties[j].name;
      if (text_is(name, "offset") || text_is(name, "easing") || text_is(name, "composite")) {
        continue;
      }
      kprop p;
      bool skip = false;
      st = make_prop(w, name, &in->properties[j].value, &p, &skip);
      if (st == GTEXT_ANIM_OK && !skip && !insert_sorted(&w->alloc, f, &p)) {
        kprop_free(&w->alloc, &p);
        st = GTEXT_ANIM_OOM;
      }
    }
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
  }
  return finish_keyframes(w, k, NULL, 0);
}

static GTEXT_Anim_Status process_indexed(GTEXT_Anim_World * w, const GTEXT_Anim_Keyframes_Argument * arg, GTEXT_Anim_Keyframes * k) {
  const GTEXT_Anim_Property_Indexed * in = arg->indexed;
  if (!in) {
    return GTEXT_ANIM_INVALID;
  }
  for (size_t i = 0; i < in->offset_count; i++) {
    if (!finite_offset(&in->offsets[i])) {
      return GTEXT_ANIM_TYPE_ERROR;
    }
  }
  /* the properties, sorted by the names they were given under (the order of "animation properties") */
  const size_t np = in->property_count;
  size_t * order = gtext_allocator_malloc(&w->alloc, (np ? np : 1) * sizeof *order);
  if (!order) {
    return GTEXT_ANIM_OOM;
  }
  for (size_t i = 0; i < np; i++) {
    size_t j = i;
    while (j > 0 && cmp_bytes(in->properties[order[j - 1]].name.data, in->properties[order[j - 1]].name.length, in->properties[i].name.data, in->properties[i].name.length) > 0) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = i;
  }
  GTEXT_Anim_Status st = GTEXT_ANIM_OK;
  /* "For each member m: skip composite, easing and offset; for each value v a new keyframe with a null offset and the pair; compute missing keyframe offsets; add them to processed keyframes" */
  double * computed = NULL;
  size_t ncomputed = 0, capcomputed = 0;
  for (size_t oi = 0; oi < np && st == GTEXT_ANIM_OK; oi++) {
    const GTEXT_Anim_Property_List_Input * m = &in->properties[order[oi]];
    if (text_is(&m->name, "offset") || text_is(&m->name, "easing") || text_is(&m->name, "composite")) {
      continue;
    }
    kprop proto;
    bool skip = false;
    st = make_prop(w, &m->name, &(GTEXT_Anim_Text){"", 0}, &proto, &skip);
    if (st != GTEXT_ANIM_OK || skip) {
      continue;
    }
    const size_t first = k->n;
    for (size_t vi = 0; vi < m->value_count && st == GTEXT_ANIM_OK; vi++) {
      kframe * f = kf_push(k);
      if (!f) {
        st = GTEXT_ANIM_OOM;
        break;
      }
      if (ncomputed == capcomputed) {
        const size_t cap = capcomputed ? capcomputed * 2 : 8;
        double * bigger = gtext_allocator_realloc(&w->alloc, computed, cap * sizeof *bigger);
        if (!bigger) {
          st = GTEXT_ANIM_OOM;
          break;
        }
        computed = bigger;
        capcomputed = cap;
      }
      computed[ncomputed++] = 0.0;
      kprop p = {0};
      p.name = dup_bytes(&w->alloc, proto.name, proto.name_length);
      p.name_length = proto.name_length;
      p.idl = dup_bytes(&w->alloc, proto.idl, proto.idl_length);
      p.idl_length = proto.idl_length;
      p.input = dup_bytes(&w->alloc, proto.input, proto.input_length);
      p.input_length = proto.input_length;
      p.value = dup_bytes(&w->alloc, m->values[vi].data ? m->values[vi].data : "", m->values[vi].length);
      p.value_length = m->values[vi].length;
      if (!p.name || !p.idl || !p.input || !p.value || !kframe_add_prop(&w->alloc, f, &p)) {
        kprop_free(&w->alloc, &p);
        st = GTEXT_ANIM_OOM;
      }
    }
    kprop_free(&w->alloc, &proto);
    if (st == GTEXT_ANIM_OK) {
      const size_t count = k->n - first;
      for (size_t vi = 0; vi < count; vi++) {
        computed[first + vi] = count > 1 ? (double)vi / (double)(count - 1) : 1.0;
      }
    }
  }
  gtext_allocator_free(&w->alloc, order);
  if (st != GTEXT_ANIM_OK) {
    gtext_allocator_free(&w->alloc, computed);
    return st;
  }
  /* "Sort processed keyframes by the computed keyframe offset of each keyframe in increasing order", stably */
  for (size_t i = 1; i < k->n; i++) {
    size_t j = i;
    while (j > 0 && computed[j - 1] > computed[j]) {
      const double td = computed[j - 1];
      computed[j - 1] = computed[j];
      computed[j] = td;
      const kframe tf = k->frames[j - 1];
      k->frames[j - 1] = k->frames[j];
      k->frames[j] = tf;
      j--;
    }
  }
  /* "Merge adjacent keyframes in processed keyframes when they have equal computed keyframe offsets": into a new array, so that a failure leaves nothing half moved (a keyframe that was moved is zeroed) */
  size_t groups = 0;
  for (size_t i = 0; i < k->n; i++) {
    groups += !(i > 0 && computed[i] == computed[i - 1]);
  }
  kframe * merged = gtext_allocator_calloc(&w->alloc, groups ? groups : 1, sizeof *merged);
  if (!merged) {
    gtext_allocator_free(&w->alloc, computed);
    return GTEXT_ANIM_OOM;
  }
  size_t g = 0;
  for (size_t i = 0; i < k->n && st == GTEXT_ANIM_OK; i++) {
    if (i > 0 && computed[i] == computed[i - 1]) {
      kframe * into = &merged[g - 1];
      kprop * bigger = gtext_allocator_realloc(&w->alloc, into->props, (into->nprops + k->frames[i].nprops + 1) * sizeof *bigger);
      if (!bigger) {
        st = GTEXT_ANIM_OOM;
        break;
      }
      into->props = bigger;
      for (size_t j = 0; j < k->frames[i].nprops; j++) {
        into->props[into->nprops++] = k->frames[i].props[j];
      }
      gtext_allocator_free(&w->alloc, k->frames[i].props);
      gtext_allocator_free(&w->alloc, k->frames[i].easing_text);
      memset(&k->frames[i], 0, sizeof k->frames[i]);
      continue;
    }
    merged[g++] = k->frames[i];
    memset(&k->frames[i], 0, sizeof k->frames[i]);
  }
  gtext_allocator_free(&w->alloc, computed);
  if (st != GTEXT_ANIM_OK) {
    for (size_t i = 0; i < g; i++) {
      kframe_free(&w->alloc, &merged[i]);
    }
    gtext_allocator_free(&w->alloc, merged);
    return st;
  }
  gtext_allocator_free(&w->alloc, k->frames);
  k->frames = merged;
  k->n = groups;
  k->cap = groups ? groups : 1;
  /* "Assign each value in offsets to the keyframe offset of the keyframe with the corresponding position, until the end of either sequence is reached" */
  for (size_t i = 0; i < in->offset_count && i < k->n; i++) {
    k->frames[i].offset_null = in->offsets[i].is_null;
    k->frames[i].offset = in->offsets[i].is_null ? 0.0 : in->offsets[i].value;
  }
  /* "easings": as given, or linear, repeated to the number of keyframes; the excess is validated */
  const size_t ne = in->easing_count;
  GTEXT_Anim_Text linear = {NULL, 0};
  for (size_t i = 0; i < k->n && st == GTEXT_ANIM_OK; i++) {
    st = set_easing_text(w, &k->frames[i], ne ? &in->easings[i % ne] : &linear);
  }
  /* "composite modes": repeated; `auto` is the default and assigned nothing */
  const size_t nc = in->composite_count;
  for (size_t i = 0; nc && i < k->n; i++) {
    k->frames[i].composite = in->composites[i % nc];
  }
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  return finish_keyframes(w, k, ne > k->n ? in->easings + k->n : NULL, ne > k->n ? ne - k->n : 0);
}

GTEXT_Anim_Status gtext_anim_keyframes_process(GTEXT_Anim_World * world, const GTEXT_Anim_Keyframes_Argument * arg, GTEXT_Anim_Keyframes ** out) {
  if (!world || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  GTEXT_Anim_Keyframes * k = kf_new(&world->alloc);
  if (!k) {
    return GTEXT_ANIM_OOM;
  }
  GTEXT_Anim_Status st = GTEXT_ANIM_OK;
  if (arg && arg->kind == GTEXT_ANIM_KEYFRAMES_ARRAY) {
    st = process_array(world, arg, k);
  }
  else if (arg && arg->kind == GTEXT_ANIM_KEYFRAMES_INDEXED) {
    st = process_indexed(world, arg, k);
  }
  else if (arg && arg->kind != GTEXT_ANIM_KEYFRAMES_NULL) {
    st = GTEXT_ANIM_INVALID;
  }
  if (st != GTEXT_ANIM_OK) {
    gtext_anim_keyframes_free(k);
    return st;
  }
  *out = k;
  return GTEXT_ANIM_OK;
}

/* ---- the effect ---------------------------------------------------------------------------------------------------------------------------------- */

struct GTEXT_Anim_Effect {
  GTEXT_Anim_World * world;
  GTEXT_Anim_Animation * animation;       ///< The animation that holds it (its timing is the animation's), or NULL.
  const void * target;
  char * pseudo;                          ///< The pseudo-element selector with two colons, or NULL.
  size_t pseudo_length;
  char * key;                             ///< The key of the pseudo-element as the tracker has it: the name without colons ("" for the element).
  size_t key_length;
  GTEXT_Anim_Composite composite;
  GTEXT_Anim_Iteration_Composite iteration;
  GTEXT_Anim_Effect_Timing timing;
  GTEXT_CSS_Easing * easing;              ///< The effect easing function (owned): `timing.easing` points at it.
  char * easing_text;                     ///< Its serialization (owned).
  size_t easing_length;
  GTEXT_Anim_Keyframes * keyframes;       ///< The keyframes (owned); NULL for an effect that is a view of a CSS animation or transition.
  GTEXT_Anim_Kind css_kind;               ///< Not 0: the effect is the view of a CSS animation or transition (its keyframes, timing and target are the style's, which this object reads).
  uint64_t css_id;
  /* the keyframes as the sampler reads them (built on demand, dropped when the keyframes change) */
  GTEXT_CSS_Keyframe * sframes;
  GTEXT_CSS_Keyframe_Property * sprops;
  const GTEXT_CSS_Easing ** seasings;
  signed char * scomposites;
  bool sampler_ready;
};

static void effect_touch(GTEXT_Anim_Effect * e) {
  anim_world_touch(e->world);
}

static void sampler_drop(GTEXT_Anim_Effect * e) {
  const GTEXT_Allocator * a = &e->world->alloc;
  gtext_allocator_free(a, e->sframes);
  gtext_allocator_free(a, e->sprops);
  gtext_allocator_free(a, (void *)e->seasings);
  gtext_allocator_free(a, e->scomposites);
  e->sframes = NULL;
  e->sprops = NULL;
  e->seasings = NULL;
  e->scomposites = NULL;
  e->sampler_ready = false;
}

static void effect_free(GTEXT_Anim_Effect * e) {
  if (!e) {
    return;
  }
  const GTEXT_Allocator * a = &e->world->alloc;
  sampler_drop(e);
  gtext_allocator_free(a, e->pseudo);
  gtext_allocator_free(a, e->key);
  gtext_css_easing_free(e->easing);
  gtext_allocator_free(a, e->easing_text);
  gtext_anim_keyframes_free(e->keyframes);
  gtext_allocator_free(a, e);
}

static GTEXT_Anim_Status effect_alloc(GTEXT_Anim_World * w, GTEXT_Anim_Effect ** out) {
  GTEXT_Anim_Effect * e = gtext_allocator_calloc(&w->alloc, 1, sizeof *e);
  if (!e) {
    return GTEXT_ANIM_OOM;
  }
  e->world = w;
  e->timing = gtext_anim_timing_default();
  GTEXT_Anim_Effect ** bigger = w->neffects == w->capeffects ? gtext_allocator_realloc(&w->alloc, w->effects, (w->capeffects ? w->capeffects * 2 : 8) * sizeof *bigger) : w->effects;
  if (!bigger) {
    gtext_allocator_free(&w->alloc, e);
    return GTEXT_ANIM_OOM;
  }
  if (w->neffects == w->capeffects) {
    w->capeffects = w->capeffects ? w->capeffects * 2 : 8;
  }
  w->effects = bigger;
  w->effects[w->neffects++] = e;
  *out = e;
  return GTEXT_ANIM_OK;
}

/// Take an effect that was added to the world's list and is not made out of it again.
static void effect_discard(GTEXT_Anim_World * w, GTEXT_Anim_Effect * e) {
  for (size_t i = 0; i < w->neffects; i++) {
    if (w->effects[i] == e) {
      memmove(&w->effects[i], &w->effects[i + 1], (w->neffects - i - 1) * sizeof *w->effects);
      w->neffects--;
      break;
    }
  }
  effect_free(e);
}

void anim_effects_free(GTEXT_Anim_World * w) {
  for (size_t i = 0; i < w->neffects; i++) {
    effect_free(w->effects[i]);
  }
  gtext_allocator_free(&w->alloc, w->effects);
  w->effects = NULL;
  w->neffects = w->capeffects = 0;
}

/// Parse the text of an `easing` member: the easing function, its serialization.
static GTEXT_Anim_Status parse_timing_easing(GTEXT_Anim_World * w, const GTEXT_Anim_Text * text, GTEXT_CSS_Easing ** easing, char ** serialized, size_t * length) {
  return keyframe_easing(w, text, easing, serialized, length);
}

/// "Pseudo-element parsing": the selector with two colons, and its key. `text` NULL is `null`.
static GTEXT_Anim_Status parse_pseudo_target(GTEXT_Anim_World * w, const GTEXT_Anim_Text * text, char ** pseudo, size_t * pseudo_length, char ** key, size_t * key_length) {
  *pseudo = NULL;
  *pseudo_length = 0;
  *key = NULL;
  *key_length = 0;
  if (!text || !text->data) {
    return GTEXT_ANIM_OK;
  }
  bool valid = false;
  GTEXT_CSS_Status cs = css_pseudo_target(anim_world_core(w), &w->alloc, text->data, text->length, key, key_length, &valid);
  if (cs == GTEXT_CSS_E_OOM) {
    return GTEXT_ANIM_OOM;
  }
  if (cs != GTEXT_CSS_OK || !valid) {
    return GTEXT_ANIM_SYNTAX_ERROR;
  }
  /* "If value is one of the legacy Selectors Level 2 single-colon selectors (':before', ':after', ':first-letter', or ':first-line'), then return the equivalent two-colon selector" */
  const bool legacy = text->length >= 2 && text->data[0] == ':' && text->data[1] != ':' &&
      ((text->length == 7 && memcmp(text->data, ":before", 7) == 0) || (text->length == 6 && memcmp(text->data, ":after", 6) == 0) ||
       (text->length == 13 && memcmp(text->data, ":first-letter", 13) == 0) || (text->length == 11 && memcmp(text->data, ":first-line", 11) == 0));
  char * copy = gtext_allocator_malloc(&w->alloc, text->length + 2);
  if (!copy) {
    gtext_allocator_free(&w->alloc, *key);
    *key = NULL;
    return GTEXT_ANIM_OOM;
  }
  size_t at = 0;
  if (legacy) {
    copy[at++] = ':';
  }
  memcpy(copy + at, text->data, text->length);
  at += text->length;
  copy[at] = '\0';
  *pseudo = copy;
  *pseudo_length = at;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_world_effect_new(GTEXT_Anim_World * w, const void * target, const GTEXT_Anim_Keyframes_Argument * keyframes, const GTEXT_Anim_Effect_Options * options,
    GTEXT_Anim_Effect ** out) {
  if (!w || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  GTEXT_Anim_Effect * e = NULL;
  GTEXT_Anim_Status st = effect_alloc(w, &e);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  e->target = target;
  /* the effect's easing function: linear */
  GTEXT_Anim_Text linear = {NULL, 0};
  st = parse_timing_easing(w, &linear, &e->easing, &e->easing_text, &e->easing_length);
  if (st == GTEXT_ANIM_OK && options && options->has_pseudo) {
    st = parse_pseudo_target(w, &options->pseudo, &e->pseudo, &e->pseudo_length, &e->key, &e->key_length);
  }
  if (st == GTEXT_ANIM_OK && options) {
    /* "Update the timing properties of an animation effect": the members that exist are validated, then assigned */
    GTEXT_Anim_Optional_Timing in = options->timing;
    GTEXT_CSS_Easing * easing = NULL;
    char * text = NULL;
    size_t tl = 0;
    if (in.present & GTEXT_ANIM_TIMING_EASING) {
      st = parse_timing_easing(w, &options->easing, &easing, &text, &tl);
      in.values.easing = easing;
    }
    if (st == GTEXT_ANIM_OK) {
      st = gtext_anim_timing_update(&e->timing, &in);
    }
    if (st == GTEXT_ANIM_OK && easing) {
      gtext_css_easing_free(e->easing);
      gtext_allocator_free(&w->alloc, e->easing_text);
      e->easing = easing;
      e->easing_text = text;
      e->easing_length = tl;
    }
    else {
      gtext_css_easing_free(easing);
      gtext_allocator_free(&w->alloc, text);
    }
    if (st == GTEXT_ANIM_OK && options->has_composite) {
      e->composite = options->composite;
    }
    if (st == GTEXT_ANIM_OK && options->has_iteration_composite) {
      e->iteration = options->iteration_composite;
    }
  }
  e->timing.easing = e->easing;
  if (st == GTEXT_ANIM_OK) {
    st = gtext_anim_keyframes_process(w, keyframes, &e->keyframes);
  }
  if (st != GTEXT_ANIM_OK) {
    effect_discard(w, e);
    return st;
  }
  *out = e;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_world_effect_copy(GTEXT_Anim_World * w, const GTEXT_Anim_Effect * src, GTEXT_Anim_Effect ** out) {
  if (!w || !src || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  GTEXT_Anim_Effect * e = NULL;
  GTEXT_Anim_Status st = effect_alloc(w, &e);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  e->target = src->target;
  e->composite = src->composite;
  e->iteration = src->iteration;
  if (src->pseudo) {
    e->pseudo = dup_bytes(&w->alloc, src->pseudo, src->pseudo_length);
    e->pseudo_length = src->pseudo_length;
    e->key = dup_bytes(&w->alloc, src->key, src->key_length);
    e->key_length = src->key_length;
    if (!e->pseudo || !e->key) {
      st = GTEXT_ANIM_OOM;
    }
  }
  GTEXT_Anim_Effect_Timing t;
  GTEXT_CSS_String easing_text = {NULL, 0};
  if (st == GTEXT_ANIM_OK) {
    st = gtext_anim_effect_get_timing(src, &t, &easing_text);
  }
  if (st == GTEXT_ANIM_OK) {
    GTEXT_Anim_Text text = {easing_text.data, easing_text.length};
    st = parse_timing_easing(w, &text, &e->easing, &e->easing_text, &e->easing_length);
    e->timing = t;
    e->timing.easing = e->easing;
  }
  if (st == GTEXT_ANIM_OK) {
    st = gtext_anim_effect_get_keyframes((GTEXT_Anim_Effect *)(uintptr_t)src, &e->keyframes);
  }
  if (st != GTEXT_ANIM_OK) {
    effect_discard(w, e);
    return st;
  }
  *out = e;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_world_effect_of(GTEXT_Anim_World * w, GTEXT_Anim_Animation * a, GTEXT_Anim_Effect ** out) {
  if (!w || !a || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = gtext_anim_animation_effect(a);
  if (*out) {
    return GTEXT_ANIM_OK;
  }
  const control_t * c = NULL;
  for (size_t i = 0; i < w->ncontrols; i++) {
    if (w->controls[i].animation == a) {
      c = &w->controls[i];
    }
  }
  GTEXT_Anim_Animation_State s;
  if (!c || c->kind == GTEXT_ANIM_KIND_SCRIPT || gtext_anim_animation_state(a, anim_world_now(w), &s) != GTEXT_ANIM_OK || !s.has_effect) {
    return GTEXT_ANIM_OK;   /* an animation with no effect (idle, `effect = null`): NULL */
  }
  GTEXT_Anim_Effect * e = NULL;
  GTEXT_Anim_Status st = effect_alloc(w, &e);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  e->css_kind = c->kind;
  e->css_id = c->id;
  e->target = c->element;
  if (c->pseudo_length) {
    e->pseudo = dup_bytes(&w->alloc, c->pseudo, c->pseudo_length);
    e->pseudo_length = c->pseudo_length;
    const size_t skip = c->pseudo_length > 2 && c->pseudo[0] == ':' && c->pseudo[1] == ':' ? 2 : 0;
    e->key = dup_bytes(&w->alloc, c->pseudo + skip, c->pseudo_length - skip);
    e->key_length = c->pseudo_length - skip;
    if (!e->pseudo || !e->key) {
      st = GTEXT_ANIM_OOM;
    }
  }
  GTEXT_Anim_Text linear = {NULL, 0};
  if (st == GTEXT_ANIM_OK) {
    st = parse_timing_easing(w, &linear, &e->easing, &e->easing_text, &e->easing_length);
  }
  if (st != GTEXT_ANIM_OK) {
    effect_discard(w, e);
    return st;
  }
  e->animation = a;
  anim_effect_put(a, e);
  *out = e;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Animation * gtext_anim_effect_animation(const GTEXT_Anim_Effect * e) {
  return e ? e->animation : NULL;
}

GTEXT_Anim_Status gtext_anim_effect_get_target(const GTEXT_Anim_Effect * e, const void ** element, GTEXT_CSS_String * pseudo) {
  if (!e || !element) {
    return GTEXT_ANIM_INVALID;
  }
  *element = e->target;
  if (pseudo) {
    *pseudo = (GTEXT_CSS_String){e->pseudo ? e->pseudo : "", e->pseudo ? e->pseudo_length : 0};
  }
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_effect_set_target(GTEXT_Anim_Effect * e, const void * element) {
  if (!e) {
    return GTEXT_ANIM_INVALID;
  }
  if (e->css_kind) {
    return GTEXT_ANIM_NOT_SUPPORTED;
  }
  e->target = element;
  effect_touch(e);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_effect_set_pseudo(GTEXT_Anim_Effect * e, const char * text, size_t length) {
  if (!e || (!text && length)) {
    return GTEXT_ANIM_INVALID;
  }
  if (e->css_kind) {
    return GTEXT_ANIM_NOT_SUPPORTED;
  }
  GTEXT_Anim_Text t = {text, length};
  char * pseudo = NULL;
  char * key = NULL;
  size_t pl = 0, kl = 0;
  const GTEXT_Anim_Status st = parse_pseudo_target(e->world, text ? &t : NULL, &pseudo, &pl, &key, &kl);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  gtext_allocator_free(&e->world->alloc, e->pseudo);
  gtext_allocator_free(&e->world->alloc, e->key);
  e->pseudo = pseudo;
  e->pseudo_length = pl;
  e->key = key;
  e->key_length = kl;
  effect_touch(e);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Composite gtext_anim_effect_get_composite(const GTEXT_Anim_Effect * e) {
  if (e && e->css_kind == GTEXT_ANIM_KIND_CSS_ANIMATION) {
    bool found = false;
    GTEXT_Anim_Info info;
    if (gtext_anim_world_find(e->world, e->css_kind, e->css_id, &found, &info) == GTEXT_ANIM_OK && found) {
      return info.composite;
    }
  }
  return e ? e->composite : GTEXT_ANIM_COMPOSITE_REPLACE;
}

GTEXT_Anim_Status gtext_anim_effect_set_composite(GTEXT_Anim_Effect * e, GTEXT_Anim_Composite c) {
  if (!e || (c != GTEXT_ANIM_COMPOSITE_REPLACE && c != GTEXT_ANIM_COMPOSITE_ADD && c != GTEXT_ANIM_COMPOSITE_ACCUMULATE)) {
    return GTEXT_ANIM_INVALID;
  }
  if (e->css_kind) {
    return GTEXT_ANIM_NOT_SUPPORTED;
  }
  e->composite = c;
  effect_touch(e);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Iteration_Composite gtext_anim_effect_get_iteration_composite(const GTEXT_Anim_Effect * e) {
  return e ? e->iteration : GTEXT_ANIM_ITERATION_REPLACE;
}

GTEXT_Anim_Status gtext_anim_effect_set_iteration_composite(GTEXT_Anim_Effect * e, GTEXT_Anim_Iteration_Composite c) {
  if (!e || (c != GTEXT_ANIM_ITERATION_REPLACE && c != GTEXT_ANIM_ITERATION_ACCUMULATE)) {
    return GTEXT_ANIM_INVALID;
  }
  e->iteration = c;
  effect_touch(e);
  return GTEXT_ANIM_OK;
}

/// The effect of a CSS animation or transition shows the timing the Animation holds; its easing is serialized by the record that owns it, and copied here when asked.
static GTEXT_Anim_Status css_view_easing(GTEXT_Anim_Effect * e) {
  bool found = false;
  GTEXT_Anim_Info info;
  GTEXT_Anim_Status st = gtext_anim_world_find(e->world, e->css_kind, e->css_id, &found, &info);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  if (!found) {
    return GTEXT_ANIM_OK;   /* the animation has left the document: the easing it had stays */
  }
  char * copy = dup_bytes(&e->world->alloc, info.easing.data ? info.easing.data : "linear", info.easing.data ? info.easing.length : 6);
  if (!copy) {
    return GTEXT_ANIM_OOM;
  }
  gtext_allocator_free(&e->world->alloc, e->easing_text);
  e->easing_text = copy;
  e->easing_length = info.easing.data ? info.easing.length : 6;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_effect_get_timing(const GTEXT_Anim_Effect * e, GTEXT_Anim_Effect_Timing * timing, GTEXT_CSS_String * easing) {
  if (!e) {
    return GTEXT_ANIM_INVALID;
  }
  if (e->css_kind) {
    const GTEXT_Anim_Status st = css_view_easing((GTEXT_Anim_Effect *)(uintptr_t)e);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
  }
  if (timing) {
    *timing = e->timing;
    if (e->animation) {
      (void)gtext_anim_animation_get_timing(e->animation, timing);
    }
  }
  if (easing) {
    *easing = (GTEXT_CSS_String){e->easing_text, e->easing_length};
  }
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_effect_update_timing(GTEXT_Anim_Effect * e, const GTEXT_Anim_Optional_Timing * input, const GTEXT_Anim_Text * easing, GTEXT_Anim_Time now) {
  if (!e || !input) {
    return GTEXT_ANIM_INVALID;
  }
  GTEXT_Anim_World * w = e->world;
  if (e->css_kind == GTEXT_ANIM_KIND_CSS_TRANSITION) {
    return GTEXT_ANIM_NOT_SUPPORTED;   /* the timing of a CSS transition is its style change event's: Transitions 2 has no rule for a script that changes it */
  }
  if (e->css_kind == GTEXT_ANIM_KIND_CSS_ANIMATION) {
    const GTEXT_Anim_Status us = css_tracker_update_animation_timing(w->tracker, anim_world_registry(w), e->css_id, input, easing ? easing->data : NULL, easing ? easing->length : 0);
    if (us == GTEXT_ANIM_OK) {
      effect_touch(e);
    }
    return us;
  }
  GTEXT_Anim_Optional_Timing in = *input;
  GTEXT_CSS_Easing * next = NULL;
  char * text = NULL;
  size_t tl = 0;
  GTEXT_Anim_Status st = GTEXT_ANIM_OK;
  if (in.present & GTEXT_ANIM_TIMING_EASING) {
    st = parse_timing_easing(w, easing ? easing : &(GTEXT_Anim_Text){NULL, 0}, &next, &text, &tl);
    in.values.easing = next;
  }
  if (st == GTEXT_ANIM_OK) {
    if (e->animation) {
      st = gtext_anim_animation_update_effect_timing(e->animation, &in, now);
      if (st == GTEXT_ANIM_OK) {
        (void)gtext_anim_animation_get_timing(e->animation, &e->timing);
      }
    }
    else {
      GTEXT_Anim_Effect_Timing t = e->timing;
      st = gtext_anim_timing_update(&t, &in);
      if (st == GTEXT_ANIM_OK) {
        GTEXT_Anim_Computed_Timing c;
        st = gtext_anim_computed_timing(&t, (GTEXT_Anim_Time){false, 0.0}, 1.0, false, &c);
      }
      if (st == GTEXT_ANIM_OK) {
        e->timing = t;
      }
    }
  }
  if (st == GTEXT_ANIM_OK && next) {
    gtext_css_easing_free(e->easing);
    gtext_allocator_free(&w->alloc, e->easing_text);
    e->easing = next;
    e->easing_text = text;
    e->easing_length = tl;
    next = NULL;
    text = NULL;
  }
  gtext_css_easing_free(next);
  gtext_allocator_free(&w->alloc, text);
  if (st == GTEXT_ANIM_OK) {
    effect_touch(e);
  }
  return st;
}

static GTEXT_Anim_Status effect_computed_timing(const GTEXT_Anim_Effect * e, GTEXT_Anim_Time now, bool endpoint_inclusive, GTEXT_Anim_Computed_Timing * out);

GTEXT_Anim_Status gtext_anim_effect_get_computed_timing(const GTEXT_Anim_Effect * e, GTEXT_Anim_Time now, GTEXT_Anim_Computed_Timing * out) {
  return effect_computed_timing(e, now, false, out);
}

static GTEXT_Anim_Status effect_computed_timing(const GTEXT_Anim_Effect * e, GTEXT_Anim_Time now, bool endpoint_inclusive, GTEXT_Anim_Computed_Timing * out) {
  if (!e || !out) {
    return GTEXT_ANIM_INVALID;
  }
  GTEXT_Anim_Effect_Timing t;
  (void)gtext_anim_effect_get_timing(e, &t, NULL);
  GTEXT_Anim_Time local = {false, 0.0};
  double rate = 1.0;
  if (e->animation) {
    GTEXT_Anim_Animation_State s;
    if (gtext_anim_animation_state(e->animation, now, &s) == GTEXT_ANIM_OK) {
      local = s.current_time;
      rate = s.playback_rate;
    }
  }
  return gtext_anim_computed_timing(&t, local, rate, endpoint_inclusive, out);
}

/* ---- the keyframes of an effect ----------------------------------------------------------------------------------------------------------------- */

static GTEXT_Anim_Keyframes * kf_clone(GTEXT_Anim_World * w, const GTEXT_Anim_Keyframes * src) {
  GTEXT_Anim_Keyframes * k = kf_new(&w->alloc);
  if (!k) {
    return NULL;
  }
  for (size_t i = 0; i < src->n; i++) {
    const kframe * f = &src->frames[i];
    kframe * c = kf_push(k);
    if (!c) {
      gtext_anim_keyframes_free(k);
      return NULL;
    }
    c->offset_null = f->offset_null;
    c->offset = f->offset;
    c->computed = f->computed;
    c->composite = f->composite;
    GTEXT_Anim_Status st = GTEXT_ANIM_OK;
    if (f->easing_text) {
      c->easing_text = dup_bytes(&w->alloc, f->easing_text, f->easing_length);
      c->easing_length = f->easing_length;
      st = c->easing_text ? GTEXT_ANIM_OK : GTEXT_ANIM_OOM;
    }
    if (st == GTEXT_ANIM_OK && f->easing) {
      /* the easing function is made again from its serialization: the text the call serializes it to is the one that is held already */
      GTEXT_Anim_Text t = {f->easing_text, f->easing_length};
      char * again = NULL;
      size_t again_length = 0;
      st = keyframe_easing(w, &t, &c->easing, &again, &again_length);
      gtext_allocator_free(&w->alloc, again);
    }
    for (size_t j = 0; j < f->nprops && st == GTEXT_ANIM_OK; j++) {
      kprop p = {0};
      p.name = dup_bytes(&w->alloc, f->props[j].name, f->props[j].name_length);
      p.name_length = f->props[j].name_length;
      p.idl = dup_bytes(&w->alloc, f->props[j].idl, f->props[j].idl_length);
      p.idl_length = f->props[j].idl_length;
      p.input = dup_bytes(&w->alloc, f->props[j].input ? f->props[j].input : "", f->props[j].input_length);
      p.input_length = f->props[j].input_length;
      p.value = dup_bytes(&w->alloc, f->props[j].value, f->props[j].value_length);
      p.value_length = f->props[j].value_length;
      if (!p.name || !p.idl || !p.input || !p.value || !kframe_add_prop(&w->alloc, c, &p)) {
        kprop_free(&w->alloc, &p);
        st = GTEXT_ANIM_OOM;
      }
    }
    if (st != GTEXT_ANIM_OK) {
      gtext_anim_keyframes_free(k);
      return NULL;
    }
  }
  return k;
}

/// The keyframes of the effect of a CSS transition: two, at 0 and 1, of the property it transitions, with the computed values of its start and its end ("the transition's effect ... a keyframe effect with two keyframes",
/// Transitions 2, "Animations"); their easing is linear (the transition's timing function is the effect's) and their composite operation is auto. A transition that has left the document has none.
static GTEXT_Anim_Status transition_keyframes(GTEXT_Anim_Effect * e, GTEXT_Anim_Keyframes ** out) {
  GTEXT_Anim_World * w = e->world;
  bool found = false;
  GTEXT_Anim_Info info;
  GTEXT_Anim_Status st = gtext_anim_world_find(w, e->css_kind, e->css_id, &found, &info);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  GTEXT_Anim_Keyframes * k = kf_new(&w->alloc);
  if (!k) {
    return GTEXT_ANIM_OOM;
  }
  for (size_t i = 0; found && i < 2 && st == GTEXT_ANIM_OK; i++) {
    kframe * f = kf_push(k);
    if (!f) {
      st = GTEXT_ANIM_OOM;
      break;
    }
    f->offset_null = false;
    f->offset = f->computed = (double)i;
    f->easing_text = dup_bytes(&w->alloc, "linear", 6);
    f->easing_length = 6;
    kprop p = {0};
    const GTEXT_CSS_String value = i ? info.end_value : info.start_value;
    p.name = dup_bytes(&w->alloc, info.name.data, info.name.length);
    p.name_length = info.name.length;
    p.value = dup_bytes(&w->alloc, value.data ? value.data : "", value.length);
    p.value_length = value.length;
    p.input = dup_bytes(&w->alloc, "", 0);
    if (!f->easing_text || !p.name || !p.value || !p.input) {
      st = GTEXT_ANIM_OOM;
    }
    else {
      st = idl_from_property(w, p.name, p.name_length, &p.idl, &p.idl_length);
    }
    if (st == GTEXT_ANIM_OK && !kframe_add_prop(&w->alloc, f, &p)) {
      st = GTEXT_ANIM_OOM;
    }
    if (st != GTEXT_ANIM_OK) {
      kprop_free(&w->alloc, &p);
    }
  }
  if (st != GTEXT_ANIM_OK) {
    gtext_anim_keyframes_free(k);
    return st;
  }
  *out = k;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_effect_get_keyframes(GTEXT_Anim_Effect * e, GTEXT_Anim_Keyframes ** out) {
  if (!e || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  if (e->css_kind == GTEXT_ANIM_KIND_CSS_TRANSITION) {
    return transition_keyframes(e, out);
  }
  if (e->css_kind) {
    return GTEXT_ANIM_NOT_SUPPORTED;
  }
  GTEXT_Anim_Keyframes * k = kf_clone(e->world, e->keyframes);
  if (!k) {
    return GTEXT_ANIM_OOM;
  }
  *out = k;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_effect_set_keyframes(GTEXT_Anim_Effect * e, const GTEXT_Anim_Keyframes_Argument * keyframes) {
  if (!e) {
    return GTEXT_ANIM_INVALID;
  }
  if (e->css_kind) {
    return GTEXT_ANIM_NOT_SUPPORTED;
  }
  GTEXT_Anim_Keyframes * k = NULL;
  const GTEXT_Anim_Status st = gtext_anim_keyframes_process(e->world, keyframes, &k);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  gtext_anim_keyframes_free(e->keyframes);
  e->keyframes = k;
  sampler_drop(e);
  effect_touch(e);
  return GTEXT_ANIM_OK;
}

/* ---- the animation's effect ---------------------------------------------------------------------------------------------------------------------- */

GTEXT_Anim_Status gtext_anim_animation_set_effect_object(GTEXT_Anim_Animation * a, GTEXT_Anim_Effect * effect, GTEXT_Anim_Time now) {
  if (!a) {
    return GTEXT_ANIM_INVALID;
  }
  GTEXT_Anim_Effect * old = gtext_anim_animation_effect(a);
  if (old == effect) {
    return GTEXT_ANIM_OK;
  }
  /* "If new effect is not null and if new effect is the associated effect of another previous animation, run the procedure to set the associated effect of previous animation to null" */
  if (effect && effect->animation && effect->animation != a) {
    const GTEXT_Anim_Status st = gtext_anim_animation_set_effect_object(effect->animation, NULL, now);
    if (st != GTEXT_ANIM_OK) {
      return st;
    }
  }
  GTEXT_Anim_Effect_Timing previous;
  const bool had = old && gtext_anim_animation_get_timing(a, &previous) == GTEXT_ANIM_OK;
  const GTEXT_Anim_Status st = gtext_anim_animation_set_effect(a, effect ? &effect->timing : NULL, now);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  anim_effect_put(a, effect);
  if (old) {
    if (had) {
      old->timing = previous;
    }
    old->animation = NULL;
    effect_touch(old);
  }
  if (effect) {
    effect->animation = a;
    effect_touch(effect);
  }
  return GTEXT_ANIM_OK;
}

/* ---- what the sampler reads ------------------------------------------------------------------------------------------------------------------------ */

/// The keyframes of an effect as the sampler reads them (css_style.h, GTEXT_CSS_Script_Effect). The arrays are built once and kept until the keyframes change.
static GTEXT_Anim_Status sampler_build(GTEXT_Anim_Effect * e) {
  if (e->sampler_ready) {
    return GTEXT_ANIM_OK;
  }
  const GTEXT_Allocator * al = &e->world->alloc;
  const GTEXT_CSS_Registry * reg = anim_world_registry(e->world);
  const GTEXT_Anim_Keyframes * k = e->keyframes;
  size_t total = 0;
  for (size_t i = 0; i < k->n; i++) {
    total += k->frames[i].nprops;
  }
  e->sframes = gtext_allocator_calloc(al, k->n + 1, sizeof *e->sframes);
  e->sprops = gtext_allocator_calloc(al, total + 1, sizeof *e->sprops);
  e->seasings = gtext_allocator_calloc(al, k->n + 1, sizeof *e->seasings);
  e->scomposites = gtext_allocator_calloc(al, k->n + 1, sizeof *e->scomposites);
  if (!e->sframes || !e->sprops || !e->seasings || !e->scomposites) {
    sampler_drop(e);
    return GTEXT_ANIM_OOM;
  }
  size_t at = 0;
  for (size_t i = 0; i < k->n; i++) {
    const kframe * f = &k->frames[i];
    e->sframes[i].offset = f->computed;
    e->sframes[i].properties = &e->sprops[at];
    e->sframes[i].property_count = f->nprops;
    for (size_t j = 0; j < f->nprops; j++) {
      GTEXT_CSS_Keyframe_Property * p = &e->sprops[at++];
      p->property = is_custom(f->props[j].name, f->props[j].name_length) ? 0 : gtext_css_registry_find_property(reg, f->props[j].name, f->props[j].name_length);
      p->name = (GTEXT_CSS_String){f->props[j].name, f->props[j].name_length};
      p->text = (GTEXT_CSS_String){f->props[j].value, f->props[j].value_length};
      p->set = NULL;
    }
    e->seasings[i] = f->easing;
    e->scomposites[i] = f->composite == GTEXT_ANIM_KF_COMPOSITE_AUTO ? (signed char)-1 : (signed char)(f->composite - GTEXT_ANIM_KF_COMPOSITE_REPLACE);
  }
  e->sampler_ready = true;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status anim_effect_sample(GTEXT_Anim_Effect * e, GTEXT_Anim_Time now, bool endpoint_inclusive, GTEXT_CSS_Script_Effect * out, bool * in_effect) {
  *in_effect = false;
  if (e->css_kind) {
    return GTEXT_ANIM_OK;   /* the view of a CSS animation or transition: the style samples it */
  }
  GTEXT_Anim_Computed_Timing c;
  GTEXT_Anim_Status st = effect_computed_timing(e, now, endpoint_inclusive, &c);
  if (st != GTEXT_ANIM_OK || !c.in_effect || !c.has_progress) {
    return st;
  }
  st = sampler_build(e);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  memset(out, 0, sizeof *out);
  out->frames = e->sframes;
  out->frame_count = e->keyframes->n;
  out->easings = e->seasings;
  out->composites = e->scomposites;
  out->composite = (GTEXT_CSS_Anim_Composite)e->composite;
  out->iteration_accumulate = e->iteration == GTEXT_ANIM_ITERATION_ACCUMULATE;
  out->progress = c.progress;
  out->current_iteration = c.has_current_iteration ? c.current_iteration : 0.0;
  out->before_flag = c.before_flag;
  *in_effect = true;
  return GTEXT_ANIM_OK;
}

const char * anim_effect_key(const GTEXT_Anim_Effect * e, size_t * length) {
  *length = e->key ? e->key_length : 0;
  return e->key ? e->key : "";
}

const void * anim_effect_target(const GTEXT_Anim_Effect * e) {
  return e->target;
}
