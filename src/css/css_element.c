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
 * The element interface and the environment (css_element.h): the guarded calls over a caller's table, the names of the callbacks, and
 * the default and the check of an environment.
 */

#include <ghoti.io/text/css/css_element.h>

#include <string.h>

#include "css_internal.h"

/* NaN and the infinities are tested by comparison: src/css takes no <math.h> (make check-css-libm). */

static bool finite_positive(double v) {
  return v == v && v > 0.0 && v < 1.0e300;
}

static const char * const callback_names[GTEXT_CSS_ELEMENT_CB_COUNT] = {
  [GTEXT_CSS_ELEMENT_CB_PARENT] = "parent",
  [GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT] = "inheritance_parent",
  [GTEXT_CSS_ELEMENT_CB_LOCAL_NAME] = "local_name",
  [GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI] = "namespace_uri",
  [GTEXT_CSS_ELEMENT_CB_ID] = "id",
  [GTEXT_CSS_ELEMENT_CB_CLASS_COUNT] = "class_count",
  [GTEXT_CSS_ELEMENT_CB_CLASS_AT] = "class_at",
  [GTEXT_CSS_ELEMENT_CB_ATTRIBUTE] = "attribute",
  [GTEXT_CSS_ELEMENT_CB_PREVIOUS_SIBLING] = "previous_sibling",
  [GTEXT_CSS_ELEMENT_CB_NEXT_SIBLING] = "next_sibling",
  [GTEXT_CSS_ELEMENT_CB_SIBLING_INDEX] = "sibling_index",
  [GTEXT_CSS_ELEMENT_CB_SIBLING_COUNT] = "sibling_count",
  [GTEXT_CSS_ELEMENT_CB_COMPARE_ORDER] = "compare_order",
  [GTEXT_CSS_ELEMENT_CB_STATE] = "state",
  [GTEXT_CSS_ELEMENT_CB_LANG] = "lang",
  [GTEXT_CSS_ELEMENT_CB_DIRECTION] = "direction",
};

const char * gtext_css_element_callback_name(GTEXT_CSS_Element_Callback callback) {
  if ((unsigned)callback >= (unsigned)GTEXT_CSS_ELEMENT_CB_COUNT) {
    return "unknown";
  }
  return callback_names[callback];
}

/// The byte offset of a callback's field in the table, and the field's size, so that `size` can be compared with where the field ends.
static size_t field_end(GTEXT_CSS_Element_Callback cb) {
#define END(field) (offsetof(GTEXT_CSS_Element_Interface, field) + sizeof(((GTEXT_CSS_Element_Interface *)0)->field))
  switch (cb) {
  case GTEXT_CSS_ELEMENT_CB_PARENT: return END(parent);
  case GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT: return END(inheritance_parent);
  case GTEXT_CSS_ELEMENT_CB_LOCAL_NAME: return END(local_name);
  case GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI: return END(namespace_uri);
  case GTEXT_CSS_ELEMENT_CB_ID: return END(id);
  case GTEXT_CSS_ELEMENT_CB_CLASS_COUNT: return END(class_count);
  case GTEXT_CSS_ELEMENT_CB_CLASS_AT: return END(class_at);
  case GTEXT_CSS_ELEMENT_CB_ATTRIBUTE: return END(attribute);
  case GTEXT_CSS_ELEMENT_CB_PREVIOUS_SIBLING: return END(previous_sibling);
  case GTEXT_CSS_ELEMENT_CB_NEXT_SIBLING: return END(next_sibling);
  case GTEXT_CSS_ELEMENT_CB_SIBLING_INDEX: return END(sibling_index);
  case GTEXT_CSS_ELEMENT_CB_SIBLING_COUNT: return END(sibling_count);
  case GTEXT_CSS_ELEMENT_CB_COMPARE_ORDER: return END(compare_order);
  case GTEXT_CSS_ELEMENT_CB_STATE: return END(state);
  case GTEXT_CSS_ELEMENT_CB_LANG: return END(lang);
  case GTEXT_CSS_ELEMENT_CB_DIRECTION: return END(direction);
  default: return (size_t)-1;
  }
#undef END
}

bool gtext_css_element_has(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Callback callback) {
  if (!iface || (unsigned)callback >= (unsigned)GTEXT_CSS_ELEMENT_CB_COUNT) {
    return false;
  }
  if (iface->size < field_end(callback)) {
    return false;
  }
  switch (callback) {
  case GTEXT_CSS_ELEMENT_CB_PARENT: return iface->parent != NULL;
  case GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT: return iface->inheritance_parent != NULL;
  case GTEXT_CSS_ELEMENT_CB_LOCAL_NAME: return iface->local_name != NULL;
  case GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI: return iface->namespace_uri != NULL;
  case GTEXT_CSS_ELEMENT_CB_ID: return iface->id != NULL;
  case GTEXT_CSS_ELEMENT_CB_CLASS_COUNT: return iface->class_count != NULL;
  case GTEXT_CSS_ELEMENT_CB_CLASS_AT: return iface->class_at != NULL;
  case GTEXT_CSS_ELEMENT_CB_ATTRIBUTE: return iface->attribute != NULL;
  case GTEXT_CSS_ELEMENT_CB_PREVIOUS_SIBLING: return iface->previous_sibling != NULL;
  case GTEXT_CSS_ELEMENT_CB_NEXT_SIBLING: return iface->next_sibling != NULL;
  case GTEXT_CSS_ELEMENT_CB_SIBLING_INDEX: return iface->sibling_index != NULL;
  case GTEXT_CSS_ELEMENT_CB_SIBLING_COUNT: return iface->sibling_count != NULL;
  case GTEXT_CSS_ELEMENT_CB_COMPARE_ORDER: return iface->compare_order != NULL;
  case GTEXT_CSS_ELEMENT_CB_STATE: return iface->state != NULL;
  case GTEXT_CSS_ELEMENT_CB_LANG: return iface->lang != NULL;
  case GTEXT_CSS_ELEMENT_CB_DIRECTION: return iface->direction != NULL;
  default: return false;
  }
}

static GTEXT_CSS_Status element_done(GTEXT_CSS_Element_Result * result, GTEXT_CSS_Status status, const char * callback) {
  if (result) {
    result->status = status;
    result->message = gtext_css_status_string(status);
    result->callback = callback;
  }
  return status;
}

static GTEXT_CSS_Status element_need(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Callback cb,
    GTEXT_CSS_Element_Result * result) {
  if (!iface) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  if (!gtext_css_element_has(iface, cb)) {
    return element_done(result, GTEXT_CSS_E_UNSUPPORTED, gtext_css_element_callback_name(cb));
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_element_check(const GTEXT_CSS_Element_Interface * iface, const GTEXT_CSS_Element_Callback * required,
    size_t count, GTEXT_CSS_Element_Result * result) {
  if (!iface || (count && !required) || iface->size < offsetof(GTEXT_CSS_Element_Interface, user) + sizeof iface->user) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  for (size_t i = 0; i < count; i++) {
    if ((unsigned)required[i] >= (unsigned)GTEXT_CSS_ELEMENT_CB_COUNT) {
      return element_done(result, GTEXT_CSS_E_INVALID, NULL);
    }
  }
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Element_Callback cb = required[i];
    bool ok = gtext_css_element_has(iface, cb);
    if (!ok && cb == GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT) {
      ok = gtext_css_element_has(iface, GTEXT_CSS_ELEMENT_CB_PARENT);
    }
    if (!ok) {
      return element_done(result, GTEXT_CSS_E_UNSUPPORTED, gtext_css_element_callback_name(cb));
    }
  }
  return element_done(result, GTEXT_CSS_OK, NULL);
}

GTEXT_CSS_Status gtext_css_element_get_parent(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    GTEXT_CSS_Element_Handle * out, GTEXT_CSS_Element_Result * result) {
  GTEXT_CSS_Status st = element_need(iface, GTEXT_CSS_ELEMENT_CB_PARENT, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!out) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  *out = iface->parent(iface->user, element);
  return element_done(result, GTEXT_CSS_OK, NULL);
}

GTEXT_CSS_Status gtext_css_element_get_inheritance_parent(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    GTEXT_CSS_Element_Handle * out, GTEXT_CSS_Element_Result * result) {
  if (iface && gtext_css_element_has(iface, GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT)) {
    if (!out) {
      return element_done(result, GTEXT_CSS_E_INVALID, NULL);
    }
    *out = iface->inheritance_parent(iface->user, element);
    return element_done(result, GTEXT_CSS_OK, NULL);
  }
  return gtext_css_element_get_parent(iface, element, out, result);
}

#define STRING_GETTER(fn, cb, field)                                                                                              \
  GTEXT_CSS_Status fn(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element, GTEXT_CSS_String * out,        \
      bool * found, GTEXT_CSS_Element_Result * result) {                                                                         \
    GTEXT_CSS_Status st = element_need(iface, cb, result);                                                                       \
    if (st != GTEXT_CSS_OK) {                                                                                                    \
      return st;                                                                                                                 \
    }                                                                                                                            \
    if (!out || !found) {                                                                                                        \
      return element_done(result, GTEXT_CSS_E_INVALID, NULL);                                                                    \
    }                                                                                                                            \
    GTEXT_CSS_String s = {NULL, 0};                                                                                              \
    *found = iface->field(iface->user, element, &s) && s.data != NULL;                                                           \
    *out = *found ? s : (GTEXT_CSS_String){NULL, 0};                                                                             \
    return element_done(result, GTEXT_CSS_OK, NULL);                                                                             \
  }

STRING_GETTER(gtext_css_element_get_local_name, GTEXT_CSS_ELEMENT_CB_LOCAL_NAME, local_name)
STRING_GETTER(gtext_css_element_get_namespace_uri, GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI, namespace_uri)
STRING_GETTER(gtext_css_element_get_id, GTEXT_CSS_ELEMENT_CB_ID, id)
STRING_GETTER(gtext_css_element_get_lang, GTEXT_CSS_ELEMENT_CB_LANG, lang)

GTEXT_CSS_Status gtext_css_element_get_class_count(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    size_t * out, GTEXT_CSS_Element_Result * result) {
  GTEXT_CSS_Status st = element_need(iface, GTEXT_CSS_ELEMENT_CB_CLASS_COUNT, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!out) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  *out = iface->class_count(iface->user, element);
  return element_done(result, GTEXT_CSS_OK, NULL);
}

GTEXT_CSS_Status gtext_css_element_get_class_at(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    size_t index, GTEXT_CSS_String * out, bool * found, GTEXT_CSS_Element_Result * result) {
  GTEXT_CSS_Status st = element_need(iface, GTEXT_CSS_ELEMENT_CB_CLASS_AT, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!out || !found) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  GTEXT_CSS_String s = {NULL, 0};
  *found = iface->class_at(iface->user, element, index, &s) && s.data != NULL;
  *out = *found ? s : (GTEXT_CSS_String){NULL, 0};
  return element_done(result, GTEXT_CSS_OK, NULL);
}

GTEXT_CSS_Status gtext_css_element_get_attribute(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out, bool * found,
    GTEXT_CSS_Element_Result * result) {
  GTEXT_CSS_Status st = element_need(iface, GTEXT_CSS_ELEMENT_CB_ATTRIBUTE, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!name || !out || !found) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  GTEXT_CSS_String s = {NULL, 0};
  *found = iface->attribute(iface->user, element, ns, name, &s) && s.data != NULL;
  *out = *found ? s : (GTEXT_CSS_String){NULL, 0};
  return element_done(result, GTEXT_CSS_OK, NULL);
}

#define HANDLE_GETTER(fn, cb, field)                                                                                              \
  GTEXT_CSS_Status fn(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,                               \
      GTEXT_CSS_Element_Handle * out, GTEXT_CSS_Element_Result * result) {                                                       \
    GTEXT_CSS_Status st = element_need(iface, cb, result);                                                                       \
    if (st != GTEXT_CSS_OK) {                                                                                                    \
      return st;                                                                                                                 \
    }                                                                                                                            \
    if (!out) {                                                                                                                  \
      return element_done(result, GTEXT_CSS_E_INVALID, NULL);                                                                    \
    }                                                                                                                            \
    *out = iface->field(iface->user, element);                                                                                   \
    return element_done(result, GTEXT_CSS_OK, NULL);                                                                             \
  }

HANDLE_GETTER(gtext_css_element_get_previous_sibling, GTEXT_CSS_ELEMENT_CB_PREVIOUS_SIBLING, previous_sibling)
HANDLE_GETTER(gtext_css_element_get_next_sibling, GTEXT_CSS_ELEMENT_CB_NEXT_SIBLING, next_sibling)

#define SIZE_GETTER(fn, cb, field)                                                                                                \
  GTEXT_CSS_Status fn(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element, size_t * out,                  \
      GTEXT_CSS_Element_Result * result) {                                                                                       \
    GTEXT_CSS_Status st = element_need(iface, cb, result);                                                                       \
    if (st != GTEXT_CSS_OK) {                                                                                                    \
      return st;                                                                                                                 \
    }                                                                                                                            \
    if (!out) {                                                                                                                  \
      return element_done(result, GTEXT_CSS_E_INVALID, NULL);                                                                    \
    }                                                                                                                            \
    *out = iface->field(iface->user, element);                                                                                   \
    return element_done(result, GTEXT_CSS_OK, NULL);                                                                             \
  }

SIZE_GETTER(gtext_css_element_get_sibling_index, GTEXT_CSS_ELEMENT_CB_SIBLING_INDEX, sibling_index)
SIZE_GETTER(gtext_css_element_get_sibling_count, GTEXT_CSS_ELEMENT_CB_SIBLING_COUNT, sibling_count)

GTEXT_CSS_Status gtext_css_element_compare_order(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle a,
    GTEXT_CSS_Element_Handle b, int * out, GTEXT_CSS_Element_Result * result) {
  GTEXT_CSS_Status st = element_need(iface, GTEXT_CSS_ELEMENT_CB_COMPARE_ORDER, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!out) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  const int c = iface->compare_order(iface->user, a, b);
  *out = c < 0 ? -1 : c > 0 ? 1 : 0;
  return element_done(result, GTEXT_CSS_OK, NULL);
}

GTEXT_CSS_Status gtext_css_element_get_state(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    uint64_t * out, GTEXT_CSS_Element_Result * result) {
  GTEXT_CSS_Status st = element_need(iface, GTEXT_CSS_ELEMENT_CB_STATE, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!out) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  *out = iface->state(iface->user, element);
  return element_done(result, GTEXT_CSS_OK, NULL);
}

GTEXT_CSS_Status gtext_css_element_get_direction(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element,
    GTEXT_CSS_Direction * out, GTEXT_CSS_Element_Result * result) {
  GTEXT_CSS_Status st = element_need(iface, GTEXT_CSS_ELEMENT_CB_DIRECTION, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!out) {
    return element_done(result, GTEXT_CSS_E_INVALID, NULL);
  }
  *out = iface->direction(iface->user, element) == GTEXT_CSS_DIRECTION_RTL ? GTEXT_CSS_DIRECTION_RTL : GTEXT_CSS_DIRECTION_LTR;
  return element_done(result, GTEXT_CSS_OK, NULL);
}

GTEXT_CSS_Environment gtext_css_environment_default(void) {
  GTEXT_CSS_Environment env;
  memset(&env, 0, sizeof env);
  env.size = sizeof env;
  env.small_viewport.width = env.large_viewport.width = env.dynamic_viewport.width = 1280.0;
  env.small_viewport.height = env.large_viewport.height = env.dynamic_viewport.height = 800.0;
  env.default_font_size = 16.0;
  env.device_pixel_ratio = 1.0;
  env.color_scheme = GTEXT_CSS_SCHEME_LIGHT;
  return env;
}

GTEXT_CSS_Status gtext_css_environment_check(const GTEXT_CSS_Environment * env, const char ** detail) {
  const char * why = NULL;
  if (!env) {
    if (detail) {
      *detail = "no environment";
    }
    return GTEXT_CSS_E_INVALID;
  }
  if (env->size < offsetof(GTEXT_CSS_Environment, default_font_size) + sizeof env->default_font_size) {
    why = "size does not cover the viewports and the default font size";
  }
  else if (!finite_positive(env->small_viewport.width) || !finite_positive(env->small_viewport.height)) {
    why = "small_viewport must be positive and finite";
  }
  else if (!finite_positive(env->large_viewport.width) || !finite_positive(env->large_viewport.height)) {
    why = "large_viewport must be positive and finite";
  }
  else if (!finite_positive(env->dynamic_viewport.width) || !finite_positive(env->dynamic_viewport.height)) {
    why = "dynamic_viewport must be positive and finite";
  }
  else if (!finite_positive(env->default_font_size)) {
    why = "default_font_size must be positive and finite";
  }
  else if (env->size >= offsetof(GTEXT_CSS_Environment, device_pixel_ratio) + sizeof env->device_pixel_ratio &&
      !finite_positive(env->device_pixel_ratio)) {
    why = "device_pixel_ratio must be positive and finite";
  }
  else if (env->size >= offsetof(GTEXT_CSS_Environment, color_scheme) + sizeof env->color_scheme &&
      env->color_scheme != GTEXT_CSS_SCHEME_LIGHT && env->color_scheme != GTEXT_CSS_SCHEME_DARK) {
    why = "color_scheme is not a scheme";
  }
  if (detail) {
    *detail = why;
  }
  return why ? GTEXT_CSS_E_INVALID : GTEXT_CSS_OK;
}
