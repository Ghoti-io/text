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
 * Computing a style (css_computed.h): the context and its initial values, the order of computation, defaulting (`initial`, `inherit`, `unset`,
 * `all`), the cascade of the members of a logical group as one property, custom properties, inheritance chains, and the accessors of a style.
 *
 * A computed value is a refcounted value set. A style holds one for every property of the registry; what it inherits or takes from the initial
 * values is retained from the parent and the context, not copied.
 */

#include <ghoti.io/text/css/css_computed.h>

#include <stdlib.h>
#include <string.h>

#include "css_compute_internal.h"
#include "css_substitute_internal.h"

#define DEFAULT_MAX_ELEMENTS 100000u
#define LIMIT_MAX_ELEMENTS ((size_t)1 << 24)
#define DEFAULT_MAX_LIST 65536u
#define LIMIT_MAX_LIST ((size_t)1 << 24)
#define DEFAULT_MAX_NODES ((size_t)1 << 20)
#define LIMIT_MAX_NODES ((size_t)1 << 28)

/* ---- names ----------------------------------------------------------------------------------- */

static const char * const class_names[GTEXT_CSS_COMPUTE_CLASS_COUNT] = {
  [GTEXT_CSS_COMPUTE_UNIMPLEMENTED] = "unimplemented",
  [GTEXT_CSS_COMPUTE_IDENTITY] = "identity",
  [GTEXT_CSS_COMPUTE_BY_TYPE] = "by-type",
  [GTEXT_CSS_COMPUTE_ALGORITHM] = "algorithm",
  [GTEXT_CSS_COMPUTE_SHORTHAND] = "shorthand",
  [GTEXT_CSS_COMPUTE_ALIAS] = "alias",
  [GTEXT_CSS_COMPUTE_CUSTOM] = "custom",
};

const char * gtext_css_compute_class_name(GTEXT_CSS_Compute_Class klass) {
  if ((unsigned)klass >= (unsigned)GTEXT_CSS_COMPUTE_CLASS_COUNT) {
    return "unknown";
  }
  return class_names[klass];
}

static const char * const algo_names[GTEXT_CSS_ALGO_COUNT] = {
  [GTEXT_CSS_ALGO_NONE] = "NONE",
  [GTEXT_CSS_ALGO_FONT_SIZE] = "FONT_SIZE",
  [GTEXT_CSS_ALGO_FONT_WEIGHT] = "FONT_WEIGHT",
  [GTEXT_CSS_ALGO_FONT_WIDTH] = "FONT_WIDTH",
  [GTEXT_CSS_ALGO_FONT_STYLE] = "FONT_STYLE",
  [GTEXT_CSS_ALGO_LINE_HEIGHT] = "LINE_HEIGHT",
  [GTEXT_CSS_ALGO_FONT_SIZE_ADJUST] = "FONT_SIZE_ADJUST",
  [GTEXT_CSS_ALGO_BORDER_WIDTH] = "BORDER_WIDTH",
  [GTEXT_CSS_ALGO_RULE_WIDTH] = "RULE_WIDTH",
  [GTEXT_CSS_ALGO_LINE_WIDTH] = "LINE_WIDTH",
  [GTEXT_CSS_ALGO_DISPLAY] = "DISPLAY",
  [GTEXT_CSS_ALGO_FLOAT] = "FLOAT",
  [GTEXT_CSS_ALGO_COLOR] = "COLOR",
  [GTEXT_CSS_ALGO_OPACITY] = "OPACITY",
  [GTEXT_CSS_ALGO_ZOOM] = "ZOOM",
  [GTEXT_CSS_ALGO_MATH_DEPTH] = "MATH_DEPTH",
  [GTEXT_CSS_ALGO_TEXT_ALIGN] = "TEXT_ALIGN",
  [GTEXT_CSS_ALGO_VOICE_PITCH] = "VOICE_PITCH",
  [GTEXT_CSS_ALGO_VOICE_BALANCE] = "VOICE_BALANCE",
  [GTEXT_CSS_ALGO_HYPHENATE_CHARS] = "HYPHENATE_CHARS",
  [GTEXT_CSS_ALGO_IMAGE_ORIENTATION] = "IMAGE_ORIENTATION",
  [GTEXT_CSS_ALGO_PAIR_DUP] = "PAIR_DUP",
  [GTEXT_CSS_ALGO_BOX4] = "BOX4",
  [GTEXT_CSS_ALGO_REPEAT] = "REPEAT",
  [GTEXT_CSS_ALGO_POSITION] = "POSITION",
  [GTEXT_CSS_ALGO_POSITION_AXIS] = "POSITION_AXIS",
  [GTEXT_CSS_ALGO_BG_SIZE] = "BG_SIZE",
  [GTEXT_CSS_ALGO_TRANSLATE] = "TRANSLATE",
  [GTEXT_CSS_ALGO_SCALE] = "SCALE",
  [GTEXT_CSS_ALGO_ROTATE] = "ROTATE",
  [GTEXT_CSS_ALGO_SHADOW] = "SHADOW",
  [GTEXT_CSS_ALGO_WRAP_FLOW] = "WRAP_FLOW",
  [GTEXT_CSS_ALGO_NUMBER_LENGTH] = "NUMBER_LENGTH",
  [GTEXT_CSS_ALGO_CUSTOM] = "CUSTOM",
};

const char * gtext_css_compute_algorithm_name(GTEXT_CSS_Compute_Algorithm algorithm) {
  if ((unsigned)algorithm >= (unsigned)GTEXT_CSS_ALGO_COUNT) {
    return "unknown";
  }
  return algo_names[algorithm];
}

GTEXT_CSS_Compute_Options gtext_css_compute_options_default(void) {
  GTEXT_CSS_Compute_Options o;
  memset(&o, 0, sizeof o);
  o.max_element_depth = DEFAULT_MAX_ELEMENTS;
  o.max_list_items = DEFAULT_MAX_LIST;
  o.max_value_nodes = DEFAULT_MAX_NODES;
  return o;
}

/* ---- the generated table ------------------------------------------------------------------------ */

const css_tbl_compute * css_compute_row(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id, GTEXT_CSS_Property_Id * generated_id) {
  const size_t ngen = gtext_css_registry_generated_property_count();
  if (id >= 1 && id <= ngen && ngen == gtext_css_tbl_compute_count) {
    if (generated_id) {
      *generated_id = id;
    }
    return &gtext_css_tbl_compute[id - 1];
  }
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(registry, id, &info) || !(info.flags & GTEXT_CSS_PROP_OVERRIDE) || ngen != gtext_css_tbl_compute_count) {
    return NULL;
  }
  /* an override has the class of the generated property of its name: the names of the pin are sorted */
  size_t lo = 0, hi = ngen;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const css_tstr ts = gtext_css_tbl_properties[mid].name;
    const size_t n = ts.len < info.name_length ? ts.len : info.name_length;
    int c = memcmp(gtext_css_tbl_pool + ts.off, info.name, n);
    if (c == 0) {
      c = ts.len < info.name_length ? -1 : ts.len > info.name_length ? 1 : 0;
    }
    if (c == 0) {
      if (generated_id) {
        *generated_id = (GTEXT_CSS_Property_Id)(mid + 1);
      }
      return &gtext_css_tbl_compute[mid];
    }
    if (c < 0) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }
  return NULL;
}

GTEXT_CSS_Compute_Class gtext_css_compute_class(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property,
    GTEXT_CSS_Compute_Algorithm * algorithm, bool * partial) {
  if (algorithm) {
    *algorithm = GTEXT_CSS_ALGO_NONE;
  }
  if (partial) {
    *partial = false;
  }
  GTEXT_CSS_Property_Info info;
  if (!registry || !gtext_css_registry_property_info(registry, property, &info)) {
    return GTEXT_CSS_COMPUTE_UNIMPLEMENTED;
  }
  const css_tbl_compute * row = css_compute_row(registry, property, NULL);
  if (!row) {
    if (info.flags & GTEXT_CSS_PROP_ALIAS) {
      return GTEXT_CSS_COMPUTE_ALIAS;
    }
    if (info.flags & GTEXT_CSS_PROP_SHORTHAND) {
      return GTEXT_CSS_COMPUTE_SHORTHAND;
    }
    return info.computed == GTEXT_CSS_COMPUTED_AS_SPECIFIED ? GTEXT_CSS_COMPUTE_IDENTITY : GTEXT_CSS_COMPUTE_BY_TYPE;
  }
  if (algorithm) {
    *algorithm = (GTEXT_CSS_Compute_Algorithm)row->algo;
  }
  if (partial) {
    *partial = (row->flags & CSS_TBL_COMPUTE_PARTIAL) != 0;
  }
  return (GTEXT_CSS_Compute_Class)row->klass;
}

bool gtext_css_compute_required(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property) {
  const css_tbl_compute * row = registry ? css_compute_row(registry, property, NULL) : NULL;
  return row && (row->flags & CSS_TBL_COMPUTE_REQUIRED) != 0;
}

size_t gtext_css_compute_order_count(void) {
  return gtext_css_tbl_compute_order_count;
}

const char * gtext_css_compute_order_at(size_t index) {
  if (index >= gtext_css_tbl_compute_order_count) {
    return NULL;
  }
  return css_tstr_c(gtext_css_tbl_properties[gtext_css_tbl_compute_order[index] - 1].name);
}

/* ---- the result ---------------------------------------------------------------------------------- */

static void result_clear(GTEXT_CSS_Compute_Result * r) {
  if (r) {
    memset(r, 0, sizeof *r);
    r->status = GTEXT_CSS_OK;
    r->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
}

GTEXT_CSS_Status css_st_fail(css_state * st, GTEXT_CSS_Status status, const char * name, const char * detail) {
  if (st->status != GTEXT_CSS_OK) {
    return st->status;
  }
  st->status = status;
  if (st->res) {
    st->res->status = status;
    st->res->message = gtext_css_status_string(status);
    st->res->property = st->current;
    st->res->detail = detail;
    st->res->name[0] = '\0';
    if (name) {
      css_copy_name(st->res->name, sizeof st->res->name, name);
    }
    else if (st->current && st->cc && st->current <= st->cc->nprops) {
      css_copy_name(st->res->name, sizeof st->res->name, st->cc->meta[st->current - 1].name);
    }
  }
  return status;
}

static GTEXT_CSS_Status result_done(GTEXT_CSS_Compute_Result * r, GTEXT_CSS_Status status) {
  if (r && r->status == GTEXT_CSS_OK) {
    r->status = status;
    r->message = gtext_css_status_string(status);
  }
  return status;
}

/* ---- styles --------------------------------------------------------------------------------------- */

static GTEXT_CSS_Computed_Style * style_new(const GTEXT_CSS_Compute_Context * cc) {
  GTEXT_Allocator a = cc->alloc;
  GTEXT_CSS_Computed_Style * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return NULL;
  }
  s->cc = cc;
  s->alloc = a;
  s->n = cc->nprops;
  s->vals = gtext_allocator_calloc(&a, cc->nprops ? cc->nprops : 1, sizeof *s->vals);
  if (!s->vals) {
    gtext_allocator_free(&a, s);
    return NULL;
  }
  return s;
}

void gtext_css_computed_free(GTEXT_CSS_Computed_Style * style) {
  if (!style) {
    return;
  }
  GTEXT_Allocator a = style->alloc;
  for (size_t i = 0; i < style->n; i++) {
    gtext_css_value_set_free(style->vals[i]);
  }
  css_cmap_release(style->custom);
  css_regvals_release(style->regvals);
  gtext_css_value_set_free(style->root.family);
  gtext_css_value_set_free(style->root.line_height);
  gtext_allocator_free(&a, style->vals);
  gtext_allocator_free(&a, style);
}

static size_t prop_index(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id id) {
  const GTEXT_CSS_Property_Id r = gtext_css_registry_resolve_alias(style->cc->registry, id);
  if (r == 0 || r > style->n) {
    return (size_t)-1;
  }
  return (size_t)r - 1;
}

GTEXT_CSS_Status gtext_css_computed_value(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Value_Set ** set) {
  if (!style) {
    return GTEXT_CSS_E_INVALID;
  }
  const size_t i = prop_index(style, property);
  if (i == (size_t)-1 || style->cc->meta[i].shorthand) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!style->vals[i]) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  if (set) {
    *set = style->vals[i];
  }
  return GTEXT_CSS_OK;
}

const GTEXT_CSS_Value * gtext_css_computed_root(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property) {
  const GTEXT_CSS_Value_Set * set = NULL;
  if (gtext_css_computed_value(style, property, &set) != GTEXT_CSS_OK) {
    return NULL;
  }
  return gtext_css_value_set_root(set);
}

GTEXT_CSS_Flow gtext_css_computed_flow(const GTEXT_CSS_Computed_Style * style) {
  GTEXT_CSS_Flow f;
  memset(&f, 0, sizeof f);
  if (style) {
    f = style->flow;
  }
  return f;
}

/* ---- reading the style under construction -------------------------------------------------------------- */

const GTEXT_CSS_Value_Set * css_st_get(css_state * st, GTEXT_CSS_Property_Id id) {
  id = gtext_css_registry_resolve_alias(st->cc->registry, id);
  if (id == 0 || id > st->cc->nprops) {
    css_st_fail(st, GTEXT_CSS_E_INTERNAL, NULL, "a computation read a property the registry does not have");
    return NULL;
  }
  if (!st->done[id - 1]) {
    css_st_fail(st, GTEXT_CSS_E_INTERNAL, st->cc->meta[id - 1].name,
        "a computation read a property of the same element that is not computed yet (tools/css/compute_order.tsv)");
    return NULL;
  }
  return st->style->vals[id - 1];
}

const GTEXT_CSS_Value * css_st_root(css_state * st, GTEXT_CSS_Property_Id id) {
  return gtext_css_value_set_root(css_st_get(st, id));
}

const GTEXT_CSS_Value_Set * css_st_parent_get(css_state * st, GTEXT_CSS_Property_Id id) {
  id = gtext_css_registry_resolve_alias(st->cc->registry, id);
  if (id == 0 || id > st->cc->nprops) {
    return NULL;
  }
  if (st->parent) {
    return st->parent->vals[id - 1];
  }
  return st->cc->meta[id - 1].initial;
}

const GTEXT_CSS_Value * css_st_parent_root(css_state * st, GTEXT_CSS_Property_Id id) {
  return gtext_css_value_set_root(css_st_parent_get(st, id));
}

bool css_val_is_keyword(const GTEXT_CSS_Value * v, const char * word) {
  if (!v || v->kind != GTEXT_CSS_VAL_KEYWORD || !v->text.data) {
    return false;
  }
  const size_t n = strlen(word);
  if (v->text.length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char a = v->text.data[i];
    if (a >= 'A' && a <= 'Z') {
      a = (char)(a - 'A' + 'a');
    }
    if (a != word[i]) {
      return false;
    }
  }
  return true;
}

/* ---- the context --------------------------------------------------------------------------------- */

static uint64_t fnv_names(const GTEXT_CSS_Registry * registry, size_t n, bool * ok) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t id = 1; id <= n; id++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(registry, (GTEXT_CSS_Property_Id)id, &info)) {
      *ok = false;
      return 0;
    }
    for (size_t i = 0; i <= info.name_length; i++) {
      h ^= (unsigned char)(i == info.name_length ? 0 : info.name[i]);
      h *= 0x100000001b3ULL;
    }
  }
  return h;
}

static char * dup_str(const GTEXT_Allocator * a, const char * s) {
  if (!s) {
    return NULL;
  }
  const size_t n = strlen(s);
  char * d = gtext_allocator_malloc((GTEXT_Allocator *)a, n + 1);
  if (d) {
    memcpy(d, s, n + 1);
  }
  return d;
}

void gtext_css_compute_context_free(GTEXT_CSS_Compute_Context * cc) {
  if (!cc) {
    return;
  }
  GTEXT_Allocator a = cc->alloc;
  for (size_t i = 0; cc->meta && i < cc->nprops; i++) {
    gtext_css_value_set_free(cc->meta[i].initial);
    gtext_css_value_set_free(cc->meta[i].initial_spec);
  }
  gtext_allocator_free(&a, cc->meta);
  gtext_allocator_free(&a, cc->groups);
  gtext_allocator_free(&a, cc->order);
  gtext_allocator_free(&a, cc->base_url);
  gtext_allocator_free(&a, cc->language);
  css_ctx_free_regs(cc);
  for (size_t i = 0; i < cc->nsyntax; i++) {
    css_compiled_syntax_free(cc->syntax_cache[i]);
  }
  gtext_allocator_free(&a, cc->syntax_cache);
  if (cc->syntax_lock_made) {
    GCU_MUTEX_DESTROY(cc->syntax_lock);
  }
  gtext_allocator_free(&a, cc);
}

const GTEXT_CSS_Registry * gtext_css_compute_context_registry(const GTEXT_CSS_Compute_Context * cc) {
  return cc ? cc->registry : NULL;
}

static const struct { int slot; const char * name; } well_known[] = {
  {CSS_P_WRITING_MODE, "writing-mode"}, {CSS_P_DIRECTION, "direction"}, {CSS_P_TEXT_ORIENTATION, "text-orientation"},
  {CSS_P_FONT_FAMILY, "font-family"}, {CSS_P_FONT_STYLE, "font-style"}, {CSS_P_FONT_WEIGHT, "font-weight"},
  {CSS_P_FONT_WIDTH, "font-width"}, {CSS_P_FONT_SIZE, "font-size"}, {CSS_P_FONT_SIZE_ADJUST, "font-size-adjust"},
  {CSS_P_LINE_HEIGHT, "line-height"}, {CSS_P_FLOAT, "float"}, {CSS_P_POSITION, "position"}, {CSS_P_DISPLAY, "display"},
  {CSS_P_COLOR, "color"}, {CSS_P_MATH_DEPTH, "math-depth"}, {CSS_P_MATH_STYLE, "math-style"}, {CSS_P_TEXT_ALIGN_ALL, "text-align-all"},
  {CSS_P_ALL, "all"}, {CSS_P_UNICODE_BIDI, "unicode-bidi"}, {CSS_P_COLOR_SCHEME, "color-scheme"}, {CSS_P_CUSTOM, "--*"},
};

/// The property of a name, 0 if there is none.
static GTEXT_CSS_Property_Id find_named(const GTEXT_CSS_Registry * registry, const char * name) {
  return gtext_css_registry_find_property(registry, name, strlen(name));
}

/// The property whose name is the member's with `-width` replaced by `-style`, for a width that a style of `none` makes 0; 0 if there is none.
static GTEXT_CSS_Property_Id style_partner(const GTEXT_CSS_Registry * registry, const char * name, size_t length) {
  static const char suffix[] = "-width";
  const size_t sl = sizeof suffix - 1;
  if (length <= sl || memcmp(name + length - sl, suffix, sl) != 0) {
    return 0;
  }
  char buf[160];
  if (length - sl + 6 + 1 > sizeof buf) {
    return 0;
  }
  memcpy(buf, name, length - sl);
  memcpy(buf + length - sl, "-style", 6);
  return gtext_css_registry_find_property(registry, buf, length - sl + 6);
}

/// Parse a property's initial value text into a set; NULL (OK) when it has none that parses.
static GTEXT_CSS_Status parse_initial(const GTEXT_CSS_Compute_Context * cc, GTEXT_CSS_Property_Id id, const char * text, size_t length,
    GTEXT_CSS_Value_Set ** out) {
  *out = NULL;
  GTEXT_CSS_Property_Info info;
  if (!text || !gtext_css_registry_property_info(cc->registry, id, &info)) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &cc->alloc;
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  GTEXT_CSS_Status st = gtext_css_parse_property_text(cc->registry, info.name, info.name_length, text, length, &po, &pv);
  if (st == GTEXT_CSS_E_OOM) {
    return st;
  }
  if (st == GTEXT_CSS_OK && pv.outcome == GTEXT_CSS_PROP_RESULT_VALID) {
    *out = pv.set;
    pv.set = NULL;
  }
  gtext_css_property_value_free(&pv);
  return GTEXT_CSS_OK;
}

static void state_init(css_state * st, const GTEXT_CSS_Compute_Context * cc, GTEXT_CSS_Compute_Result * res) {
  memset(st, 0, sizeof *st);
  st->cc = cc;
  st->env = &cc->env;
  st->res = res;
  st->status = GTEXT_CSS_OK;
}

static GTEXT_CSS_Status run_pipeline(css_state * st);

/// Whether two computed values are the same (by their serialization).
static bool same_value(const GTEXT_CSS_Value_Set * a, const GTEXT_CSS_Value_Set * b) {
  if (a == b) {
    return true;
  }
  if (!a || !b) {
    return false;
  }
  const GTEXT_CSS_Value * ra = gtext_css_value_set_root(a);
  const GTEXT_CSS_Value * rb = gtext_css_value_set_root(b);
  if (!ra || !rb) {
    return ra == rb;
  }
  char *sa = NULL, *sb = NULL;
  size_t la = 0, lb = 0;
  if (gtext_css_serialize_value_buffer(ra, NULL, &sa, &la) != GTEXT_CSS_OK) {
    return false;
  }
  if (gtext_css_serialize_value_buffer(rb, NULL, &sb, &lb) != GTEXT_CSS_OK) {
    gtext_css_serialize_buffer_free(NULL, sa);
    return false;
  }
  const bool same = la == lb && memcmp(sa, sb, la) == 0;
  gtext_css_serialize_buffer_free(NULL, sa);
  gtext_css_serialize_buffer_free(NULL, sb);
  return same;
}

/// Compute a probe style (no declarations) in `env` under a parent; the context is `building`.
static GTEXT_CSS_Status probe_style(GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Environment * env, const GTEXT_CSS_Computed_Style * parent,
    GTEXT_CSS_Computed_Style ** out) {
  css_state st;
  state_init(&st, cc, NULL);
  st.env = env;
  st.parent = parent;
  st.probing = true;
  GTEXT_CSS_Computed_Style * style = style_new(cc);
  if (!style) {
    return GTEXT_CSS_E_OOM;
  }
  st.style = style;
  st.done = gtext_allocator_calloc(&cc->alloc, cc->nprops ? cc->nprops : 1, 1);
  st.busy = gtext_allocator_calloc(&cc->alloc, cc->nprops ? cc->nprops : 1, 1);
  st.own = gtext_allocator_calloc(&cc->alloc, cc->nprops ? cc->nprops : 1, sizeof *st.own);
  if (!st.done || !st.busy || !st.own) {
    gtext_allocator_free(&cc->alloc, st.done);
    gtext_allocator_free(&cc->alloc, st.busy);
    gtext_allocator_free(&cc->alloc, st.own);
    gtext_css_computed_free(style);
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Status status = run_pipeline(&st);
  css_sub_free(st.sub);
  gtext_allocator_free(&cc->alloc, st.done);
  gtext_allocator_free(&cc->alloc, st.busy);
  gtext_allocator_free(&cc->alloc, st.own);
  if (status != GTEXT_CSS_OK) {
    gtext_css_computed_free(style);
    return status;
  }
  *out = style;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_compute_context_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Environment * env,
    const GTEXT_CSS_Compute_Options * options, GTEXT_CSS_Compute_Context ** out, GTEXT_CSS_Compute_Result * result) {
  result_clear(result);
  if (!registry || !out) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  GTEXT_CSS_Environment e = gtext_css_environment_default();
  if (env) {
    const char * detail = NULL;
    GTEXT_CSS_Status chk = gtext_css_environment_check(env, &detail);
    if (chk != GTEXT_CSS_OK) {
      if (result) {
        result->detail = detail;
      }
      return result_done(result, chk);
    }
    memcpy(&e, env, env->size < sizeof e ? env->size : sizeof e);
    e.size = sizeof e;
  }
  GTEXT_CSS_Compute_Options o = options ? *options : gtext_css_compute_options_default();
  if (o.max_element_depth == 0) {
    o.max_element_depth = DEFAULT_MAX_ELEMENTS;
  }
  if (o.max_element_depth > LIMIT_MAX_ELEMENTS) {
    o.max_element_depth = LIMIT_MAX_ELEMENTS;
  }
  if (o.max_list_items == 0) {
    o.max_list_items = DEFAULT_MAX_LIST;
  }
  if (o.max_list_items > LIMIT_MAX_LIST) {
    o.max_list_items = LIMIT_MAX_LIST;
  }
  if (o.max_value_nodes == 0) {
    o.max_value_nodes = DEFAULT_MAX_NODES;
  }
  if (o.max_value_nodes > LIMIT_MAX_NODES) {
    o.max_value_nodes = LIMIT_MAX_NODES;
  }
  const GTEXT_Allocator * al = (options && options->allocator) ? options->allocator : css_registry_allocator(registry);
  GTEXT_Allocator alloc = *al;
  o.allocator = NULL;

  const size_t ngen = gtext_css_registry_generated_property_count();
  bool names_ok = true;
  if (ngen != gtext_css_tbl_compute_count || fnv_names(registry, ngen, &names_ok) != gtext_css_tbl_compute_names_hash || !names_ok) {
    if (result) {
      result->detail = "the generated computed-value table is not the one of the registry's properties (run tools/css/gen-compute-tables.py)";
    }
    return result_done(result, GTEXT_CSS_E_INTERNAL);
  }

  GTEXT_CSS_Compute_Context * cc = gtext_allocator_calloc(&alloc, 1, sizeof *cc);
  if (!cc) {
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  cc->registry = registry;
  cc->alloc = alloc;
  cc->options = o;
  if (cc->options.custom_hash_seed == 0) {
    /* no clock and no process state are read: the addresses of this allocation, the stack and the registry (randomized by the system) are the seed */
    uint64_t seed = (uint64_t)(uintptr_t)cc * 0x9E3779B97F4A7C15ULL;
    seed ^= ((uint64_t)(uintptr_t)&o + (uint64_t)(uintptr_t)registry) * 0xC2B2AE3D27D4EB4FULL;
    seed ^= seed >> 29;
    cc->options.custom_hash_seed = seed ? seed : 0x5851F42D4C957F2DULL;
  }
  cc->env = e;
  if (options && options->elements) {
    memset(&cc->iface, 0, sizeof cc->iface);
    memcpy(&cc->iface, options->elements, options->elements->size < sizeof cc->iface ? options->elements->size : sizeof cc->iface);
    cc->iface.size = options->elements->size < sizeof cc->iface ? options->elements->size : sizeof cc->iface;
    cc->has_iface = true;
  }
  cc->options.elements = NULL;
  cc->sub = cc->options.substitute;
  cc->rules = cc->options.property_rules;
  if (GCU_MUTEX_CREATE(cc->syntax_lock) != 0) {
    gtext_css_compute_context_free(cc);
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  cc->syntax_lock_made = true;
  cc->nprops = gtext_css_registry_property_count(registry);
  cc->meta = gtext_allocator_calloc(&alloc, cc->nprops ? cc->nprops : 1, sizeof *cc->meta);
  cc->base_url = dup_str(&alloc, e.base_url);
  cc->language = dup_str(&alloc, e.language);
  if (!cc->meta || (e.base_url && !cc->base_url) || (e.language && !cc->language)) {
    gtext_css_compute_context_free(cc);
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  cc->env.base_url = cc->base_url;
  cc->env.language = cc->language;

  /* the table: one row of meta for each property */
  for (size_t i = 0; i < cc->nprops; i++) {
    css_pmeta * m = &cc->meta[i];
    const GTEXT_CSS_Property_Id id = (GTEXT_CSS_Property_Id)(i + 1);
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(registry, id, &info)) {
      gtext_css_compute_context_free(cc);
      return result_done(result, GTEXT_CSS_E_INTERNAL);
    }
    m->name = info.name;
    m->name_length = info.name_length;
    m->generated = (info.flags & GTEXT_CSS_PROP_GENERATED) != 0;
    m->inherited = info.inherited == GTEXT_CSS_INHERIT_YES;
    m->shorthand = (info.flags & GTEXT_CSS_PROP_SHORTHAND) != 0;
    m->group = -1;
    m->alias_of = (info.flags & GTEXT_CSS_PROP_ALIAS) ? gtext_css_registry_resolve_alias(registry, id) : 0;
    GTEXT_CSS_Compute_Algorithm algo = GTEXT_CSS_ALGO_NONE;
    bool partial = false;
    m->klass = (uint8_t)gtext_css_compute_class(registry, id, &algo, &partial);
    m->algo = (uint8_t)algo;
    m->flags = partial ? CSS_TBL_COMPUTE_PARTIAL : 0;
  }
  for (size_t w = 0; w < sizeof well_known / sizeof *well_known; w++) {
    cc->pid[well_known[w].slot] = find_named(registry, well_known[w].name);
  }

  /* the logical groups */
  cc->groups = gtext_allocator_calloc(&alloc, 32, sizeof *cc->groups);
  if (!cc->groups) {
    gtext_css_compute_context_free(cc);
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  for (size_t i = 0; i < cc->nprops; i++) {
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(registry, (GTEXT_CSS_Property_Id)(i + 1), &info);
    if (!info.logical_group) {
      continue;
    }
    size_t g = 0;
    for (; g < cc->ngroups; g++) {
      if (strcmp(cc->groups[g].name, info.logical_group) == 0) {
        break;
      }
    }
    if (g == cc->ngroups) {
      if (g == 32) {
        continue;
      }
      css_group * grp = &cc->groups[g];
      grp->name = info.logical_group;
      const size_t gl = strlen(info.logical_group);
      const size_t nm = gtext_css_registry_logical_group_size(info.logical_group, gl);
      css_lmember lm0;
      bool ok = nm > 0 && nm <= 8;
      for (size_t k = 0; ok && k < nm; k++) {
        const GTEXT_CSS_Property_Id member = gtext_css_registry_logical_group_member(registry, info.logical_group, gl, k);
        GTEXT_CSS_Property_Info mi;
        if (!member || !gtext_css_registry_property_info(registry, member, &mi) || !css_logical_describe(info.logical_group, mi.name, mi.name_length, &lm0)) {
          ok = false;
          break;
        }
        grp->kind = (GTEXT_CSS_Logical_Kind)lm0.kind;
        grp->members[k] = member;
        if (!lm0.logical) {
          grp->physical[lm0.slot] = member;
        }
      }
      if (ok) {
        const size_t slots = grp->kind == GTEXT_CSS_LOGICAL_AXES ? 2 : 4;
        for (size_t s = 0; s < slots; s++) {
          if (!grp->physical[s]) {
            ok = false;
          }
        }
      }
      if (!ok) {
        memset(grp, 0, sizeof *grp);
        grp->name = info.logical_group;
        grp->kind = GTEXT_CSS_LOGICAL_NONE;
      }
      else {
        grp->nmembers = nm;
      }
      cc->ngroups++;
    }
    (void)g;
  }
  for (size_t g = 0; g < cc->ngroups; g++) {
    css_group * grp = &cc->groups[g];
    if (grp->kind == GTEXT_CSS_LOGICAL_NONE) {
      continue;
    }
    for (size_t k = 0; k < grp->nmembers; k++) {
      GTEXT_CSS_Property_Info mi;
      css_lmember lm;
      gtext_css_registry_property_info(registry, grp->members[k], &mi);
      css_logical_describe(grp->name, mi.name, mi.name_length, &lm);
      css_pmeta * m = &cc->meta[grp->members[k] - 1];
      m->group = (int32_t)g;
      m->member_kind = lm.logical;
      m->member_slot = lm.slot;
    }
  }
  /* the style of the same side that makes a width 0 */
  for (size_t i = 0; i < cc->nprops; i++) {
    css_pmeta * m = &cc->meta[i];
    if (m->klass == GTEXT_CSS_COMPUTE_ALGORITHM && (m->algo == GTEXT_CSS_ALGO_BORDER_WIDTH || m->algo == GTEXT_CSS_ALGO_RULE_WIDTH)) {
      m->partner = style_partner(registry, m->name, m->name_length);
    }
  }

  /* the order of computation: the table's, then every other property by id */
  cc->order = gtext_allocator_calloc(&alloc, cc->nprops ? cc->nprops : 1, sizeof *cc->order);
  if (!cc->order) {
    gtext_css_compute_context_free(cc);
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  unsigned char * seen = gtext_allocator_calloc(&alloc, cc->nprops ? cc->nprops : 1, 1);
  if (!seen) {
    gtext_css_compute_context_free(cc);
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  for (size_t k = 0; k < gtext_css_tbl_compute_order_count; k++) {
    const GTEXT_CSS_Property_Id id = gtext_css_tbl_compute_order[k];
    if (id && id <= cc->nprops && !seen[id - 1]) {
      seen[id - 1] = 1;
      cc->order[cc->norder++] = id;
    }
  }
  cc->nfirst = cc->norder;
  for (size_t i = 0; i < cc->nprops; i++) {
    const css_pmeta * m = &cc->meta[i];
    if (!seen[i] && !m->shorthand && !m->alias_of && m->klass != GTEXT_CSS_COMPUTE_SHORTHAND && m->klass != GTEXT_CSS_COMPUTE_ALIAS &&
        m->klass != GTEXT_CSS_COMPUTE_CUSTOM) {
      cc->order[cc->norder++] = (GTEXT_CSS_Property_Id)(i + 1);
    }
  }
  gtext_allocator_free(&alloc, seen);

  /* the initial values: parsed, then computed in a probe style of a root, then in two more: one in the other color scheme and one below it */
  for (size_t i = 0; i < cc->nprops; i++) {
    css_pmeta * m = &cc->meta[i];
    if (m->shorthand || m->alias_of || m->klass == GTEXT_CSS_COMPUTE_CUSTOM) {
      continue;
    }
    GTEXT_CSS_Property_Id gen = 0;
    const css_tbl_compute * row = css_compute_row(registry, (GTEXT_CSS_Property_Id)(i + 1), &gen);
    const char * text = NULL;
    size_t tl = 0;
    if (row && row->initial_len) {
      text = gtext_css_tbl_compute_pool + row->initial_off;
      tl = row->initial_len;
    }
    else {
      GTEXT_CSS_Property_Info info;
      gtext_css_registry_property_info(registry, (GTEXT_CSS_Property_Id)(i + 1), &info);
      text = info.initial;
      tl = text ? strlen(text) : 0;
    }
    if (cc->pid[CSS_P_FONT_FAMILY] && i + 1 == cc->pid[CSS_P_FONT_FAMILY] && e.default_font_family) {
      text = e.default_font_family;
      tl = strlen(text);
    }
    GTEXT_CSS_Status st = parse_initial(cc, (GTEXT_CSS_Property_Id)(i + 1), text, tl, &m->initial_spec);
    if (st != GTEXT_CSS_OK) {
      gtext_css_compute_context_free(cc);
      return result_done(result, st);
    }
    m->initial_dependent = true;
  }
  cc->building = true;
  GTEXT_CSS_Computed_Style *base = NULL, *other = NULL, *child = NULL;
  GTEXT_CSS_Status st = probe_style(cc, &cc->env, NULL, &base);
  GTEXT_CSS_Environment perturbed = cc->env;
  /* the one thing an initial value can depend on that a probe of a root does not show: the color scheme (the system colors are the element's), which a
     context's environment cannot make constant per element; the viewport, the pixel ratio and the default font size are the context's own and the same
     for every style, so an initial value that depends on them is computed once */
  perturbed.color_scheme = cc->env.color_scheme == GTEXT_CSS_SCHEME_LIGHT ? GTEXT_CSS_SCHEME_DARK : GTEXT_CSS_SCHEME_LIGHT;
  if (st == GTEXT_CSS_OK) {
    st = probe_style(cc, &perturbed, NULL, &other);
  }
  if (st == GTEXT_CSS_OK) {
    st = probe_style(cc, &cc->env, base, &child);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_computed_free(base);
    gtext_css_computed_free(other);
    gtext_css_computed_free(child);
    cc->building = false;
    gtext_css_compute_context_free(cc);
    if (result) {
      result->detail = "an initial value could not be computed";
    }
    return result_done(result, st == GTEXT_CSS_E_UNSUPPORTED ? GTEXT_CSS_E_INTERNAL : st);
  }
  for (size_t i = 0; i < cc->nprops; i++) {
    css_pmeta * m = &cc->meta[i];
    if (!base->vals[i]) {
      m->initial_dependent = false;
      continue;
    }
    const bool dependent = !same_value(base->vals[i], other->vals[i]) || !same_value(base->vals[i], child->vals[i]);
    m->initial_dependent = dependent;
    m->initial = gtext_css_value_set_retain(base->vals[i]);
  }
  /* the registered custom properties: the initial value of each is computed in the root's probe, being computationally independent */
  {
    css_state rst;
    state_init(&rst, cc, NULL);
    rst.style = base;
    rst.probing = true;
    rst.done = gtext_allocator_malloc(&alloc, cc->nprops ? cc->nprops : 1);
    rst.busy = gtext_allocator_calloc(&alloc, cc->nprops ? cc->nprops : 1, 1);
    rst.own = gtext_allocator_calloc(&alloc, cc->nprops ? cc->nprops : 1, sizeof *rst.own);
    if (rst.done) {
      memset(rst.done, 1, cc->nprops ? cc->nprops : 1);
    }
    if (!rst.done || !rst.busy || !rst.own) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      st = css_ctx_make_regs(cc, &rst);
    }
    gtext_allocator_free(&alloc, rst.done);
    gtext_allocator_free(&alloc, rst.busy);
    gtext_allocator_free(&alloc, rst.own);
  }
  gtext_css_computed_free(base);
  gtext_css_computed_free(other);
  gtext_css_computed_free(child);
  cc->building = false;
  if (st != GTEXT_CSS_OK) {
    if (result) {
      result->detail = "the initial value of a registered custom property could not be computed";
    }
    gtext_css_compute_context_free(cc);
    return result_done(result, st == GTEXT_CSS_E_UNSUPPORTED ? GTEXT_CSS_E_INTERNAL : st);
  }
  *out = cc;
  return result_done(result, GTEXT_CSS_OK);
}


/* ---- one property -------------------------------------------------------------------------------- */

typedef enum { SRC_NONE = 0, SRC_ITEM, SRC_ALL } src_kind;

typedef struct {
  src_kind kind;
  size_t index;             ///< The item's index in the block.
} css_source;

/// Whether `all` applies to a property: every property but `direction`, `unicode-bidi` and the custom properties.
static bool all_applies(const css_state * st, GTEXT_CSS_Property_Id id) {
  return id != st->cc->pid[CSS_P_DIRECTION] && id != st->cc->pid[CSS_P_UNICODE_BIDI] && id != st->cc->pid[CSS_P_CUSTOM];
}

/// The declaration that decides a property: the later of its own and of `all`.
static css_source pick_source(const css_state * st, GTEXT_CSS_Property_Id id) {
  css_source s = {SRC_NONE, 0};
  const uint32_t own = st->own[id - 1];
  const uint32_t all = all_applies(st, id) ? st->all_pos : 0;
  if (own && own > all) {
    s.kind = SRC_ITEM;
    s.index = own - 1;
  }
  else if (all) {
    s.kind = SRC_ALL;
    s.index = all - 1;
  }
  return s;
}

/// The effective position of a member's declaration (0 for none), and whether it is `all`'s.
static uint32_t effective(const css_state * st, GTEXT_CSS_Property_Id id, bool * from_all) {
  const css_source s = pick_source(st, id);
  *from_all = s.kind == SRC_ALL;
  return s.kind == SRC_NONE ? 0 : (uint32_t)s.index + 1;
}

static GTEXT_CSS_Status compute_value(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, bool declared,
    GTEXT_CSS_Value_Set ** out) {
  const css_pmeta * m = &st->cc->meta[id - 1];
  st->work++;
  switch ((GTEXT_CSS_Compute_Class)m->klass) {
  case GTEXT_CSS_COMPUTE_IDENTITY:
    /* the specified value: but a calculation of numbers is a number (z-index: calc(1.5) is 2) */
    return css_cv_by_type_ex(st, src, id, CV_MATH_NUMBERS_ONLY, out);
  case GTEXT_CSS_COMPUTE_BY_TYPE:
    return css_cv_by_type(st, src, id, out);
  case GTEXT_CSS_COMPUTE_ALGORITHM:
    return css_algo_compute(st, id, m, src, out);
  case GTEXT_CSS_COMPUTE_UNIMPLEMENTED:
    if (declared) {
      return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, m->name, "the computed value of this property is not implemented");
    }
    *out = gtext_css_value_set_retain(src);
    return GTEXT_CSS_OK;
  default:
    return css_st_fail(st, GTEXT_CSS_E_INTERNAL, m->name, "a property that has no computed value of its own was computed");
  }
}

static GTEXT_CSS_Status initial_value(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Value_Set ** out) {
  const css_pmeta * m = &st->cc->meta[id - 1];
  *out = NULL;
  if (!st->cc->building && !m->initial_dependent && m->initial) {
    *out = gtext_css_value_set_retain(m->initial);
    return GTEXT_CSS_OK;
  }
  if (m->initial_spec) {
    return compute_value(st, id, m->initial_spec, false, out);
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_st_inherit(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Value_Set ** out);

static GTEXT_CSS_Status inherit_value(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Value_Set ** out) {
  if (st->parent) {
    *out = gtext_css_value_set_retain(st->parent->vals[id - 1]);
    return GTEXT_CSS_OK;
  }
  return initial_value(st, id, out);
}

GTEXT_CSS_Status css_st_inherit(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Value_Set ** out) {
  return inherit_value(st, id, out);
}

static GTEXT_CSS_Status from_wide(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Wide_Keyword wide, GTEXT_CSS_Value_Set ** out) {
  const css_pmeta * m = &st->cc->meta[id - 1];
  switch (wide) {
  case GTEXT_CSS_WIDE_INITIAL:
    return initial_value(st, id, out);
  case GTEXT_CSS_WIDE_INHERIT:
    return inherit_value(st, id, out);
  case GTEXT_CSS_WIDE_UNSET:
    return m->inherited ? inherit_value(st, id, out) : initial_value(st, id, out);
  default:
    return css_st_fail(st, GTEXT_CSS_E_INVALID, m->name, "revert and revert-layer are the cascade's: they are resolved before a style is computed");
  }
}

/* ---- values with a substitution function in them ----------------------------------------------------------------- */

/// Invalid at computed-value time: "the property's inherited value or its initial value depending on whether the property is inherited or not, respectively, as
/// if the property's value had been specified as the unset keyword" (Values 5, Appendix A, "Invalid Substitution").
static GTEXT_CSS_Status invalid_at_computed_time(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Value_Set ** out) {
  return st->cc->meta[id - 1].inherited ? inherit_value(st, id, out) : initial_value(st, id, out);
}

/// Whether a parsed value holds a URL (or a function that takes one) made from an attr() ("Using an attr()-tainted value as or in a <url> makes a declaration
/// invalid at computed-value time ... as are functions that contain an attr()-tainted value"). `taint` are the tainted ranges of the text the value was read from.
static GTEXT_CSS_Status tainted_url(css_state * st, const GTEXT_CSS_Value * root, const css_range * taint, size_t ntaint, bool * found) {
  *found = false;
  if (!root || !ntaint) {
    return GTEXT_CSS_OK;
  }
  typedef struct {
    const GTEXT_CSS_Value * v;
    size_t n, i;
  } fr;
  GTEXT_Allocator a = st->cc->alloc;
  size_t cap = 16, len = 0;
  fr * stack = gtext_allocator_malloc(&a, cap * sizeof *stack);
  if (!stack) {
    return GTEXT_CSS_E_OOM;
  }
  stack[len++] = (fr){root, 1, 0};
  while (len && !*found) {
    fr * f = &stack[len - 1];
    if (f->i >= f->n) {
      len--;
      continue;
    }
    const GTEXT_CSS_Value * v = &f->v[f->i++];
    bool overlaps_self = false;
    for (size_t k = 0; k < ntaint; k++) {
      overlaps_self = overlaps_self || (v->span.offset < taint[k].end && v->span.end > taint[k].begin);
    }
    if (v->kind == GTEXT_CSS_VAL_URL && overlaps_self) {
      *found = true;
    }
    if (v->kind == GTEXT_CSS_VAL_FUNCTION && v->text.data && overlaps_self) {
      /* a function that takes a URL: what is tainted among its arguments, a string or a url or a function that builds one (not a color, a number ...), is the URL
         made of it ("functions that contain an attr()-tainted value are tainted") */
      static const char * const names[] = {"url", "src", "image", "image-set", "-webkit-image-set", "cross-fade", "-webkit-cross-fade", "element", "paint"};
      bool takes_url = false;
      const GTEXT_CSS_String fname = {v->text.data, v->text.length};
      for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        takes_url = takes_url || css_ascii_ieq(&fname, names[k]);
      }
      if (!takes_url) {
        /* the text of a function value may be written `name(`: compare up to the parenthesis */
        size_t nl = 0;
        while (nl < v->text.length && v->text.data[nl] != '(') {
          nl++;
        }
        const GTEXT_CSS_String head = {v->text.data, nl};
        for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
          takes_url = takes_url || css_ascii_ieq(&head, names[k]);
        }
      }
      /* the arguments, looking through the lists that group them: every one of them, however many (a list of options has no bound) */
      size_t wcap = 32, nwork = 0;
      const GTEXT_CSS_Value ** work = takes_url ? gtext_allocator_malloc(&a, wcap * sizeof *work) : NULL;
      if (takes_url && !work) {
        gtext_allocator_free(&a, stack);
        return GTEXT_CSS_E_OOM;
      }
      for (size_t c = 0; takes_url && c < v->child_count; c++) {
        if (nwork == wcap) {
          const GTEXT_CSS_Value ** g = gtext_allocator_realloc(&a, work, wcap * 2 * sizeof *g);
          if (!g) {
            gtext_allocator_free(&a, work);
            gtext_allocator_free(&a, stack);
            return GTEXT_CSS_E_OOM;
          }
          work = g;
          wcap *= 2;
        }
        work[nwork++] = &v->children[c];
      }
      while (nwork && !*found) {
        const GTEXT_CSS_Value * arg = work[--nwork];
        if (arg->kind == GTEXT_CSS_VAL_LIST) {
          for (size_t c = 0; c < arg->child_count; c++) {
            if (nwork == wcap) {
              const GTEXT_CSS_Value ** g = gtext_allocator_realloc(&a, work, wcap * 2 * sizeof *g);
              if (!g) {
                gtext_allocator_free(&a, work);
                gtext_allocator_free(&a, stack);
                return GTEXT_CSS_E_OOM;
              }
              work = g;
              wcap *= 2;
            }
            work[nwork++] = &arg->children[c];
          }
          continue;
        }
        if (arg->kind != GTEXT_CSS_VAL_STRING && arg->kind != GTEXT_CSS_VAL_URL && arg->kind != GTEXT_CSS_VAL_FUNCTION && arg->kind != GTEXT_CSS_VAL_PENDING &&
            arg->kind != GTEXT_CSS_VAL_UNPARSED && arg->kind != GTEXT_CSS_VAL_TOKEN) {
          continue;
        }
        for (size_t k = 0; k < ntaint; k++) {
          if (arg->span.offset < taint[k].end && arg->span.end > taint[k].begin) {
            *found = true;
          }
        }
      }
      gtext_allocator_free(&a, work);
    }
    if (v->child_count && !*found) {
      if (len == cap) {
        fr * g = gtext_allocator_realloc(&a, stack, cap * 2 * sizeof *g);
        if (!g) {
          gtext_allocator_free(&a, stack);
          return GTEXT_CSS_E_OOM;
        }
        stack = g;
        cap *= 2;
      }
      stack[len++] = (fr){v->children, v->child_count, 0};
    }
  }
  gtext_allocator_free(&a, stack);
  return GTEXT_CSS_OK;
}

/// A parsed value, applied to the property: a typed value is computed as any is; a CSS-wide keyword is "determined as if that keyword were its specified value all
/// along"; anything else is invalid at computed-value time. (`revert` and its kin cannot be resolved here, the cascade being over: they are taken as `unset`.)
static GTEXT_CSS_Status apply_parsed(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Property_Outcome outcome, GTEXT_CSS_Wide_Keyword wide,
    const GTEXT_CSS_Value_Set * set, GTEXT_CSS_Value_Set ** out) {
  switch (outcome) {
  case GTEXT_CSS_PROP_RESULT_VALID:
    return compute_value(st, id, set, true, out);
  case GTEXT_CSS_PROP_RESULT_WIDE:
    if (wide == GTEXT_CSS_WIDE_INITIAL || wide == GTEXT_CSS_WIDE_INHERIT || wide == GTEXT_CSS_WIDE_UNSET) {
      return from_wide(st, id, wide, out);
    }
    return invalid_at_computed_time(st, id, out);
  default:
    return invalid_at_computed_time(st, id, out);
  }
}

/// Replace the functions of a text, and read what is left as the value of `name` (a property, or a shorthand's name). `*done` is false when the value is the
/// guaranteed-invalid value or does not parse (it is invalid at computed-value time).
static GTEXT_CSS_Status substitute_and_parse_text(css_state * st, const char * name, size_t name_length, const char * input, size_t input_length,
    GTEXT_CSS_Property_Value * pv, bool * done) {
  *done = false;
  memset(pv, 0, sizeof *pv);
  css_sub * sub = st->sub;
  if (!sub) {
    return css_st_fail(st, GTEXT_CSS_E_INTERNAL, name, "a value with a substitution function in it was reached with no substitution state");
  }
  css_rope * rope = NULL;
  GTEXT_CSS_Status status = css_sub_substitute_text(sub, name, name_length, input, input_length, &rope);
  if (status != GTEXT_CSS_OK) {
    return status == GTEXT_CSS_E_OOM || status == GTEXT_CSS_E_UNSUPPORTED || status == GTEXT_CSS_E_LIMIT_DEPTH ? status : css_st_fail(st, status, name, NULL);
  }
  if (!rope) {
    return GTEXT_CSS_OK;
  }
  char * text = NULL;
  size_t length = 0;
  css_range * taint = NULL;
  size_t ntaint = 0;
  status = css_sub_flatten(sub, rope, &text, &length, &taint, &ntaint);
  css_rope_release(rope);
  if (status != GTEXT_CSS_OK) {
    return css_st_fail(st, status, name, NULL);
  }
  if (!text) {
    return GTEXT_CSS_OK; /* too long */
  }
  GTEXT_Allocator a = st->cc->alloc;
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &a;
  status = gtext_css_parse_property_text(st->cc->registry, name, name_length, text, length, &po, pv);
  if (status == GTEXT_CSS_OK && pv->outcome == GTEXT_CSS_PROP_RESULT_VALID && ntaint) {
    bool bad = false;
    status = tainted_url(st, gtext_css_value_set_root(pv->set), taint, ntaint, &bad);
    if (status == GTEXT_CSS_OK && bad) {
      gtext_css_property_value_free(pv);
      pv->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
    }
  }
  gtext_allocator_free(&a, text);
  gtext_allocator_free(&a, taint);
  if (status != GTEXT_CSS_OK) {
    gtext_css_property_value_free(pv);
    return status == GTEXT_CSS_E_OOM ? status : css_st_fail(st, status, name, "a value could not be read once its functions were replaced");
  }
  *done = pv->outcome == GTEXT_CSS_PROP_RESULT_VALID || pv->outcome == GTEXT_CSS_PROP_RESULT_WIDE;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status substitute_and_parse(css_state * st, const char * name, size_t name_length, const GTEXT_CSS_Value_Set * pending, GTEXT_CSS_Property_Value * pv,
    bool * done) {
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(pending);
  return substitute_and_parse_text(st, name, name_length, root && root->text.data ? root->text.data : "", root && root->text.data ? root->text.length : 0, pv, done);
}

static css_short_result * shorthand_lookup(css_state * st, size_t declaration) {
  for (size_t i = 0; i < st->nshort; i++) {
    if (st->shorts[i].declaration == declaration) {
      return &st->shorts[i];
    }
  }
  return NULL;
}

/// "To determine the computed value of cycle(), first evaluate each argument as if it were the sole value of the property ... called Cn. Then, compare the property's
/// inherited value with each Cn. For the earliest Cn that matches the inherited value, the computed value of cycle() is Cn+1. If the match was the last argument in the
/// list, or there was no match, the computed value of cycle() is the computed value that the first argument represents." `cands` are the Cn; `*out` is one, retained.
static GTEXT_CSS_Status cycle_pick(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Value_Set * const * cands, size_t n, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * inherited = NULL;
  GTEXT_CSS_Status status = inherit_value(st, id, &inherited);
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  size_t pick = 0;
  for (size_t k = 0; k < n; k++) {
    st->work++;
    if (same_value(cands[k], inherited)) {
      pick = k + 1 < n ? k + 1 : 0;
      break;
    }
  }
  gtext_css_value_set_free(inherited);
  *out = gtext_css_value_set_retain(cands[pick]);
  return GTEXT_CSS_OK;
}

/// The value of `cycle()` on a longhand: each argument is the whole value of the property.
static GTEXT_CSS_Status compute_cycle_longhand(css_state * st, GTEXT_CSS_Property_Id id, const css_cycle_arg * args, size_t n, GTEXT_CSS_Value_Set ** out) {
  const css_pmeta * m = &st->cc->meta[id - 1];
  GTEXT_Allocator a = st->cc->alloc;
  GTEXT_CSS_Value_Set ** cands = gtext_allocator_calloc(&a, n, sizeof *cands);
  if (!cands) {
    return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  bool all = true;
  for (size_t k = 0; k < n && status == GTEXT_CSS_OK && all; k++) {
    GTEXT_CSS_Property_Value pv;
    bool done = false;
    status = substitute_and_parse_text(st, m->name, m->name_length, args[k].text, args[k].length, &pv, &done);
    if (status != GTEXT_CSS_OK) {
      break;
    }
    if (!done || pv.outcome != GTEXT_CSS_PROP_RESULT_VALID) {
      all = false; /* an argument that is no value of the property makes the declaration invalid */
    }
    else {
      status = compute_value(st, id, pv.set, true, &cands[k]);
    }
    gtext_css_property_value_free(&pv);
  }
  if (status == GTEXT_CSS_OK) {
    status = all ? cycle_pick(st, id, cands, n, out) : invalid_at_computed_time(st, id, out);
  }
  for (size_t k = 0; k < n; k++) {
    gtext_css_value_set_free(cands[k]);
  }
  gtext_allocator_free(&a, cands);
  return status;
}

static GTEXT_CSS_Status compute_pending(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Declaration_Item * it, GTEXT_CSS_Value_Set ** out) {
  const css_pmeta * m = &st->cc->meta[id - 1];
  *out = NULL;
  GTEXT_CSS_Status status;
  if (it->property == st->cc->pid[CSS_P_ALL] || (it->shorthand && it->shorthand == st->cc->pid[CSS_P_ALL])) {
    /* `all: var(--x)`: the value takes only a CSS-wide keyword, which is known once the functions are replaced; once for the declaration, however many properties read it */
    if (!st->all_state) {
      GTEXT_CSS_Property_Value apv;
      bool adone = false;
      status = substitute_and_parse(st, "all", 3, it->set, &apv, &adone);
      if (status != GTEXT_CSS_OK) {
        return status;
      }
      st->all_state = adone && apv.outcome == GTEXT_CSS_PROP_RESULT_WIDE ? 2 : 1;
      st->all_wide = apv.wide;
      gtext_css_property_value_free(&apv);
    }
    return st->all_state == 2 ? apply_parsed(st, id, GTEXT_CSS_PROP_RESULT_WIDE, st->all_wide, NULL, out) : invalid_at_computed_time(st, id, out);
  }
  if (it->shorthand) {
    /* "after substitution, the shorthand must be parsed and the longhands must be given their appropriate values at that point": once for the declaration,
       however many longhands read it */
    css_short_result * r = shorthand_lookup(st, it->declaration);
    if (!r) {
      if (st->nshort == st->capshort) {
        const size_t cap = st->capshort ? st->capshort * 2 : 4;
        GTEXT_Allocator a = st->cc->alloc;
        css_short_result * g = gtext_allocator_realloc(&a, st->shorts, cap * sizeof *g);
        if (!g) {
          return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
        }
        st->shorts = g;
        st->capshort = cap;
      }
      GTEXT_CSS_Property_Info info;
      if (!gtext_css_registry_property_info(st->cc->registry, it->shorthand, &info)) {
        return css_st_fail(st, GTEXT_CSS_E_INTERNAL, m->name, "the shorthand of a pending value is not in the registry");
      }
      css_short_result fresh;
      memset(&fresh, 0, sizeof fresh);
      fresh.declaration = it->declaration;
      GTEXT_CSS_Property_Value pv;
      bool done = false;
      bool is_cycle = false, cycle_invalid = false;
      css_cycle_arg * cargs = NULL;
      size_t ncargs = 0;
      status = css_sub_cycle(st->sub, it->set, &is_cycle, &cycle_invalid, &cargs, &ncargs);
      if (status != GTEXT_CSS_OK) {
        return css_st_fail(st, status, info.name, NULL);
      }
      if (is_cycle) {
        /* "If cycle() is used on a shorthand property, it sets each of its longhands to a cycle() value with arguments corresponding to what the longhand would have
           received had each of the original cycle() arguments been the sole value of the shorthand": the shorthand is expanded once for each argument */
        memset(&pv, 0, sizeof pv);
        GTEXT_Allocator a = st->cc->alloc;
        fresh.cycle = gtext_allocator_calloc(&a, ncargs ? ncargs : 1, sizeof *fresh.cycle);
        if (!fresh.cycle) {
          css_cycle_args_free(st->sub, cargs, ncargs);
          return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
        }
        fresh.invalid = cycle_invalid;
        for (size_t k = 0; k < ncargs && !fresh.invalid && status == GTEXT_CSS_OK; k++) {
          GTEXT_CSS_Property_Value apv;
          bool adone = false;
          status = substitute_and_parse_text(st, info.name, info.name_length, cargs[k].text, cargs[k].length, &apv, &adone);
          if (status != GTEXT_CSS_OK) {
            break;
          }
          if (!adone || apv.outcome != GTEXT_CSS_PROP_RESULT_VALID) {
            fresh.invalid = true;
          }
          else {
            GTEXT_CSS_Shorthand_Result sr;
            status = gtext_css_shorthand_expand(st->cc->registry, &apv, &a, &fresh.cycle[k], &sr);
            if (status == GTEXT_CSS_E_UNSUPPORTED || status == GTEXT_CSS_E_INVALID) {
              fresh.invalid = true;
              status = GTEXT_CSS_OK;
            }
            else if (status == GTEXT_CSS_OK) {
              fresh.ncycle = k + 1;
            }
          }
          gtext_css_property_value_free(&apv);
        }
        css_cycle_args_free(st->sub, cargs, ncargs);
        fresh.ncycle = ncargs;
        if (status != GTEXT_CSS_OK) {
          for (size_t k = 0; k < ncargs; k++) {
            gtext_css_longhand_list_free(fresh.cycle[k]);
          }
          gtext_allocator_free(&a, fresh.cycle);
          return css_st_fail(st, status, info.name, NULL);
        }
        st->shorts[st->nshort++] = fresh;
        r = &st->shorts[st->nshort - 1];
        goto have_result;
      }
      css_cycle_args_free(st->sub, cargs, ncargs);
      status = substitute_and_parse(st, info.name, info.name_length, it->set, &pv, &done);
      if (status != GTEXT_CSS_OK) {
        return status;
      }
      if (!done) {
        fresh.invalid = true;
      }
      else if (pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
        fresh.wide = pv.wide;
      }
      else {
        GTEXT_CSS_Shorthand_Result sr;
        GTEXT_Allocator a = st->cc->alloc;
        status = gtext_css_shorthand_expand(st->cc->registry, &pv, &a, &fresh.list, &sr);
        if (status == GTEXT_CSS_E_UNSUPPORTED || status == GTEXT_CSS_E_INVALID) {
          fresh.invalid = true;
          status = GTEXT_CSS_OK;
        }
      }
      gtext_css_property_value_free(&pv);
      if (status != GTEXT_CSS_OK) {
        return css_st_fail(st, status, info.name, NULL);
      }
      st->shorts[st->nshort++] = fresh;
      r = &st->shorts[st->nshort - 1];
    }
  have_result:
    if (r->invalid) {
      return invalid_at_computed_time(st, id, out);
    }
    if (r->cycle) {
      GTEXT_Allocator a = st->cc->alloc;
      GTEXT_CSS_Value_Set ** cands = gtext_allocator_calloc(&a, r->ncycle ? r->ncycle : 1, sizeof *cands);
      if (!cands) {
        return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
      }
      bool all = true;
      status = GTEXT_CSS_OK;
      for (size_t k = 0; k < r->ncycle && status == GTEXT_CSS_OK && all; k++) {
        bool found = false;
        GTEXT_CSS_Longhand lh;
        for (size_t i = 0; gtext_css_longhand_list_at(r->cycle[k], i, &lh); i++) {
          if (gtext_css_registry_resolve_alias(st->cc->registry, lh.property) == id) {
            found = true;
            if (lh.outcome != GTEXT_CSS_PROP_RESULT_VALID) {
              all = false;
            }
            else {
              status = compute_value(st, id, lh.set, true, &cands[k]);
            }
            break;
          }
        }
        all = all && found;
      }
      if (status == GTEXT_CSS_OK) {
        status = all ? cycle_pick(st, id, cands, r->ncycle, out) : invalid_at_computed_time(st, id, out);
      }
      for (size_t k = 0; k < r->ncycle; k++) {
        gtext_css_value_set_free(cands[k]);
      }
      gtext_allocator_free(&a, cands);
      return status;
    }
    if (r->wide != GTEXT_CSS_WIDE_NONE) {
      return apply_parsed(st, id, GTEXT_CSS_PROP_RESULT_WIDE, r->wide, NULL, out);
    }
    GTEXT_CSS_Longhand lh;
    for (size_t i = 0; gtext_css_longhand_list_at(r->list, i, &lh); i++) {
      if (gtext_css_registry_resolve_alias(st->cc->registry, lh.property) == id) {
        return apply_parsed(st, id, lh.outcome, lh.wide, lh.set, out);
      }
    }
    return invalid_at_computed_time(st, id, out);
  }
  {
    bool is_cycle = false, cycle_invalid = false;
    css_cycle_arg * cargs = NULL;
    size_t ncargs = 0;
    status = css_sub_cycle(st->sub, it->set, &is_cycle, &cycle_invalid, &cargs, &ncargs);
    if (status != GTEXT_CSS_OK) {
      return css_st_fail(st, status, m->name, NULL);
    }
    if (is_cycle) {
      status = cycle_invalid ? invalid_at_computed_time(st, id, out) : compute_cycle_longhand(st, id, cargs, ncargs, out);
      css_cycle_args_free(st->sub, cargs, ncargs);
      return status;
    }
  }
  GTEXT_CSS_Property_Value pv;
  bool done = false;
  status = substitute_and_parse(st, m->name, m->name_length, it->set, &pv, &done);
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  if (!done) {
    gtext_css_property_value_free(&pv);
    return invalid_at_computed_time(st, id, out);
  }
  status = apply_parsed(st, id, pv.outcome, pv.wide, pv.set, out);
  gtext_css_property_value_free(&pv);
  return status;
}

/// Compute the value of `id` from the declaration `src` (or from nothing), into `*out`.
static GTEXT_CSS_Status compute_from_source(css_state * st, GTEXT_CSS_Property_Id id, css_source src, GTEXT_CSS_Value_Set ** out) {
  const css_pmeta * m = &st->cc->meta[id - 1];
  *out = NULL;
  if (src.kind == SRC_NONE) {
    return m->inherited ? inherit_value(st, id, out) : initial_value(st, id, out);
  }
  GTEXT_CSS_Declaration_Item it;
  if (!gtext_css_block_item(st->block, src.index, &it)) {
    return css_st_fail(st, GTEXT_CSS_E_INTERNAL, m->name, "the block changed while it was read");
  }
  switch (it.outcome) {
  case GTEXT_CSS_PROP_RESULT_WIDE:
    return from_wide(st, id, it.wide, out);
  case GTEXT_CSS_PROP_RESULT_VALID:
    return compute_value(st, id, it.set, true, out);
  default:
    return compute_pending(st, id, &it, out);
  }
}

static void set_entry(css_state * st, GTEXT_CSS_Property_Id id, GTEXT_CSS_Value_Set * set) {
  gtext_css_value_set_free(st->style->vals[id - 1]);
  st->style->vals[id - 1] = set;
  st->done[id - 1] = 1;
}

/// The members of a logical group that share the slot of `id`, computed once: the declaration that is last among them decides.
static GTEXT_CSS_Status compute_group(css_state * st, GTEXT_CSS_Property_Id id) {
  const css_pmeta * m = &st->cc->meta[id - 1];
  const css_group * g = &st->cc->groups[m->group];
  /* the slot of this member, and the winner among the members of that slot */
  css_lmember lm;
  GTEXT_CSS_Property_Info info;
  gtext_css_registry_property_info(st->cc->registry, id, &info);
  css_logical_describe(g->name, info.name, info.name_length, &lm);
  const size_t slot = css_logical_slot_of(&lm, &st->style->flow);
  GTEXT_CSS_Property_Id winner = 0;
  uint32_t win_eff = 0;
  bool win_all = false;
  for (size_t k = 0; k < g->nmembers; k++) {
    const GTEXT_CSS_Property_Id member = g->members[k];
    GTEXT_CSS_Property_Info mi;
    css_lmember ml;
    gtext_css_registry_property_info(st->cc->registry, member, &mi);
    css_logical_describe(g->name, mi.name, mi.name_length, &ml);
    if (css_logical_slot_of(&ml, &st->style->flow) != slot) {
      continue;
    }
    bool from_all = false;
    const uint32_t eff = effective(st, member, &from_all);
    if (eff == 0) {
      continue;
    }
    const bool better = eff > win_eff || (eff == win_eff && from_all && win_all && ml.logical == 0 && st->cc->meta[winner - 1].member_kind != 0);
    if (!winner || better) {
      winner = member;
      win_eff = eff;
      win_all = from_all;
    }
  }
  const GTEXT_CSS_Property_Id deciding = winner ? winner : id;
  st->current = deciding;
  GTEXT_CSS_Value_Set * set = NULL;
  const css_source src = pick_source(st, deciding);
  GTEXT_CSS_Status status = compute_from_source(st, deciding, src, &set);
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  for (size_t k = 0; k < g->nmembers; k++) {
    const GTEXT_CSS_Property_Id member = g->members[k];
    GTEXT_CSS_Property_Info mi;
    css_lmember ml;
    gtext_css_registry_property_info(st->cc->registry, member, &mi);
    css_logical_describe(g->name, mi.name, mi.name_length, &ml);
    if (css_logical_slot_of(&ml, &st->style->flow) == slot && !st->done[member - 1]) {
      set_entry(st, member, gtext_css_value_set_retain(set));
    }
  }
  gtext_css_value_set_free(set);
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status compute_property_inner(css_state * st, GTEXT_CSS_Property_Id id) {
  const css_pmeta * m = &st->cc->meta[id - 1];
  st->current = id;
  st->random_index = 0;
  if (m->group >= 0 && st->cc->groups[m->group].kind != GTEXT_CSS_LOGICAL_NONE) {
    return compute_group(st, id);
  }
  GTEXT_CSS_Value_Set * set = NULL;
  GTEXT_CSS_Status status = compute_from_source(st, id, pick_source(st, id), &set);
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  set_entry(st, id, set);
  return GTEXT_CSS_OK;
}

static void read_flow(css_state * st);
static void root_fill(css_state * st, bool after_line_height);

/// Compute a property (once), with what the others need to be able to be computed after it: the root's font when it is the font size or the line height, the flow
/// when it completes it. A property whose value reaches itself through a custom property (a registered `<length>` in `em` read by `font-size`) is found by `busy`.
static GTEXT_CSS_Status compute_property(css_state * st, GTEXT_CSS_Property_Id id) {
  if (st->done[id - 1]) {
    return GTEXT_CSS_OK;
  }
  st->busy[id - 1] = 1;
  GTEXT_CSS_Status status = compute_property_inner(st, id);
  const bool in_cycle = st->busy[id - 1] == 2;
  st->busy[id - 1] = 0;
  if (status == GTEXT_CSS_OK && in_cycle && st->done[id - 1]) {
    /* something it read (a registered `<length>` in `em`, through var()) reached it again: the property is in a dependency cycle, "invalid at computed-value time"
       whatever the value it read became */
    GTEXT_CSS_Value_Set * inv = NULL;
    status = invalid_at_computed_time(st, id, &inv);
    if (status == GTEXT_CSS_OK) {
      set_entry(st, id, inv);
    }
  }
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  if (id == cc->pid[CSS_P_FONT_SIZE]) {
    root_fill(st, false);
  }
  else if (id == cc->pid[CSS_P_LINE_HEIGHT]) {
    root_fill(st, true);
  }
  else if ((id == cc->pid[CSS_P_WRITING_MODE] || id == cc->pid[CSS_P_DIRECTION] || id == cc->pid[CSS_P_TEXT_ORIENTATION]) && cc->pid[CSS_P_WRITING_MODE] &&
      cc->pid[CSS_P_DIRECTION] && cc->pid[CSS_P_TEXT_ORIENTATION] && st->done[cc->pid[CSS_P_WRITING_MODE] - 1] && st->done[cc->pid[CSS_P_DIRECTION] - 1] &&
      st->done[cc->pid[CSS_P_TEXT_ORIENTATION] - 1]) {
    read_flow(st);
    if (st->status != GTEXT_CSS_OK) {
      return st->status;
    }
  }
  return st->status;
}

GTEXT_CSS_Status css_st_ensure(css_state * st, GTEXT_CSS_Property_Id id, bool * cycle) {
  *cycle = false;
  id = gtext_css_registry_resolve_alias(st->cc->registry, id);
  if (id == 0 || id > st->cc->nprops || st->done[id - 1]) {
    return GTEXT_CSS_OK;
  }
  if (st->busy[id - 1]) {
    st->busy[id - 1] = 2; /* it is part of a cycle: compute_property() finds it when it comes back */
    *cycle = true;
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Property_Id saved = st->current;
  const GTEXT_CSS_Status status = compute_property(st, id);
  st->current = saved;
  return status;
}

bool css_st_is_done(const css_state * st, GTEXT_CSS_Property_Id id) {
  return id && id <= st->cc->nprops && st->done[id - 1];
}

/* ---- the flow and the root ----------------------------------------------------------------------------- */

static void read_flow(css_state * st) {
  GTEXT_CSS_Flow f;
  memset(&f, 0, sizeof f);
  const GTEXT_CSS_Value * wm = css_st_root(st, st->cc->pid[CSS_P_WRITING_MODE]);
  const GTEXT_CSS_Value * dir = css_st_root(st, st->cc->pid[CSS_P_DIRECTION]);
  const GTEXT_CSS_Value * to = css_st_root(st, st->cc->pid[CSS_P_TEXT_ORIENTATION]);
  if (css_val_is_keyword(wm, "vertical-rl")) {
    f.writing_mode = GTEXT_CSS_WRITING_MODE_VERTICAL_RL;
  }
  else if (css_val_is_keyword(wm, "vertical-lr")) {
    f.writing_mode = GTEXT_CSS_WRITING_MODE_VERTICAL_LR;
  }
  else if (css_val_is_keyword(wm, "sideways-rl")) {
    f.writing_mode = GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL;
  }
  else if (css_val_is_keyword(wm, "sideways-lr")) {
    f.writing_mode = GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR;
  }
  f.direction = css_val_is_keyword(dir, "rtl") ? GTEXT_CSS_DIRECTION_RTL : GTEXT_CSS_DIRECTION_LTR;
  if (css_val_is_keyword(to, "upright")) {
    f.text_orientation = GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT;
  }
  else if (css_val_is_keyword(to, "sideways")) {
    f.text_orientation = GTEXT_CSS_TEXT_ORIENTATION_SIDEWAYS;
  }
  st->style->flow = f;
}

void css_st_font_query_of(const GTEXT_CSS_Computed_Style * style, double size_px, GTEXT_CSS_Font_Query * q) {
  memset(q, 0, sizeof *q);
  const GTEXT_CSS_Compute_Context * cc = style->cc;
  q->element = style->element;
  q->size_px = size_px;
  q->weight = 400.0;
  q->width_percent = 100.0;
  q->vertical = style->flow.writing_mode != GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB;
  if (cc->pid[CSS_P_FONT_FAMILY]) {
    q->family = gtext_css_value_set_root(style->vals[cc->pid[CSS_P_FONT_FAMILY] - 1]);
  }
  if (cc->pid[CSS_P_FONT_WEIGHT]) {
    const GTEXT_CSS_Value * w = gtext_css_value_set_root(style->vals[cc->pid[CSS_P_FONT_WEIGHT] - 1]);
    if (w && (w->kind == GTEXT_CSS_VAL_NUMBER || w->kind == GTEXT_CSS_VAL_INTEGER)) {
      q->weight = w->number;
    }
  }
  if (cc->pid[CSS_P_FONT_WIDTH]) {
    const GTEXT_CSS_Value * w = gtext_css_value_set_root(style->vals[cc->pid[CSS_P_FONT_WIDTH] - 1]);
    if (w && w->kind == GTEXT_CSS_VAL_PERCENTAGE) {
      q->width_percent = w->number;
    }
  }
  if (cc->pid[CSS_P_FONT_STYLE]) {
    const GTEXT_CSS_Value * v = gtext_css_value_set_root(style->vals[cc->pid[CSS_P_FONT_STYLE] - 1]);
    if (v && v->kind == GTEXT_CSS_VAL_KEYWORD) {
      q->italic = !css_val_is_keyword(v, "normal");
    }
    else if (v && v->kind == GTEXT_CSS_VAL_LIST && v->child_count >= 1) {
      q->italic = true;
      if (v->child_count >= 2 && v->children[1].kind == GTEXT_CSS_VAL_DIMENSION) {
        q->oblique_degrees = v->children[1].number;
      }
    }
  }
}

/// The root's font and line height, once they are computed: for `rem` and the `r` units.
static void root_fill(css_state * st, bool after_line_height) {
  if (st->parent) {
    return;
  }
  GTEXT_CSS_Computed_Style * s = st->style;
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  if (!after_line_height) {
    GTEXT_CSS_Font_Query q;
    const GTEXT_CSS_Value * fs = css_st_root(st, cc->pid[CSS_P_FONT_SIZE]);
    s->root.font_px = fs && fs->kind == GTEXT_CSS_VAL_DIMENSION ? fs->number : st->env->default_font_size;
    css_st_font_query_of(s, s->root.font_px, &q);
    s->root.weight = q.weight;
    s->root.width_percent = q.width_percent;
    s->root.italic = q.italic;
    s->root.oblique_degrees = q.oblique_degrees;
    s->root.vertical = q.vertical;
    gtext_css_value_set_free(s->root.family);
    s->root.family = cc->pid[CSS_P_FONT_FAMILY] ? gtext_css_value_set_retain(s->vals[cc->pid[CSS_P_FONT_FAMILY] - 1]) : NULL;
    s->root.valid = true;
  }
  else {
    gtext_css_value_set_free(s->root.line_height);
    s->root.line_height = cc->pid[CSS_P_LINE_HEIGHT] ? gtext_css_value_set_retain(s->vals[cc->pid[CSS_P_LINE_HEIGHT] - 1]) : NULL;
  }
}

/* ---- the pipeline -------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status run_pipeline(css_state * st) {
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  GTEXT_CSS_Computed_Style * style = st->style;
  style->element = st->element;
  const size_t nitems = st->block ? gtext_css_block_count(st->block) : 0;
  for (size_t i = 0; i < nitems; i++) {
    GTEXT_CSS_Declaration_Item it;
    if (!gtext_css_block_item(st->block, i, &it)) {
      return css_st_fail(st, GTEXT_CSS_E_INTERNAL, NULL, "the block changed while it was read");
    }
    if (it.property == cc->pid[CSS_P_ALL]) {
      st->all_pos = (uint32_t)(i + 1);
    }
    else if (it.property && it.property != cc->pid[CSS_P_CUSTOM] && it.property <= cc->nprops) {
      st->own[it.property - 1] = (uint32_t)(i + 1);
    }
  }
  if (st->parent) {
    style->root = st->parent->root;
    gtext_css_value_set_retain(style->root.family);
    gtext_css_value_set_retain(style->root.line_height);
    /* the parent's own references are the ones retained above; the struct copy shares them */
  }
  /* the custom properties first: any property may read them. The map starts as the parent's, shared; the registered ones as the parent's or their initial values */
  style->custom = css_cmap_retain(st->parent ? st->parent->custom : NULL);
  GTEXT_CSS_Status early = css_style_init_regvals(style, st->parent);
  if (early == GTEXT_CSS_OK) {
    early = css_sub_new(st, &st->sub);
  }
  if (early == GTEXT_CSS_OK && st->sub) {
    st->current = 0; /* an error in a custom property names none of the properties computed before it */
    early = css_sub_customs(st->sub);
  }
  if (early != GTEXT_CSS_OK) {
    return st->status != GTEXT_CSS_OK ? st->status : css_st_fail(st, early, NULL, NULL);
  }
  /* the flow first: a logical property is mapped by it */
  static const int flow_props[] = {CSS_P_WRITING_MODE, CSS_P_DIRECTION, CSS_P_TEXT_ORIENTATION};
  for (size_t k = 0; k < sizeof flow_props / sizeof *flow_props; k++) {
    if (cc->pid[flow_props[k]]) {
      GTEXT_CSS_Status status = compute_property(st, cc->pid[flow_props[k]]);
      if (status != GTEXT_CSS_OK) {
        return status;
      }
    }
  }
  if (cc->pid[CSS_P_WRITING_MODE] && cc->pid[CSS_P_DIRECTION] && cc->pid[CSS_P_TEXT_ORIENTATION]) {
    read_flow(st);
    if (st->status != GTEXT_CSS_OK) {
      return st->status;
    }
  }
  for (size_t k = 0; k < cc->norder; k++) {
    const GTEXT_CSS_Property_Id id = cc->order[k];
    GTEXT_CSS_Status status = compute_property(st, id);
    if (status != GTEXT_CSS_OK) {
      return status;
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- computing ---------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status compute_style_with(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Declaration_Block * block,
    const GTEXT_CSS_Computed_Style * parent, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Computed_Style ** out,
    GTEXT_CSS_Compute_Result * result);

GTEXT_CSS_Status gtext_css_compute_style(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Declaration_Block * block,
    const GTEXT_CSS_Computed_Style * parent, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Computed_Style ** out,
    GTEXT_CSS_Compute_Result * result) {
  return compute_style_with(context, block, parent, element, context && context->has_iface ? &context->iface : NULL, out, result);
}

static GTEXT_CSS_Status compute_style_with(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Declaration_Block * block,
    const GTEXT_CSS_Computed_Style * parent, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Computed_Style ** out,
    GTEXT_CSS_Compute_Result * result) {
  result_clear(result);
  if (!context || !out) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  if ((block && css_block_registry(block) != context->registry) || (parent && parent->cc != context)) {
    if (result) {
      result->detail = "the block or the parent style was made for another registry or context";
    }
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  css_state st;
  state_init(&st, context, result);
  st.block = block;
  st.parent = parent;
  st.element = element;
  st.iface = iface;
  GTEXT_CSS_Computed_Style * style = style_new(context);
  GTEXT_Allocator a = context->alloc;
  if (!style) {
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  st.style = style;
  st.done = gtext_allocator_calloc(&a, context->nprops ? context->nprops : 1, 1);
  st.busy = gtext_allocator_calloc(&a, context->nprops ? context->nprops : 1, 1);
  st.own = gtext_allocator_calloc(&a, context->nprops ? context->nprops : 1, sizeof *st.own);
  if (!st.done || !st.busy || !st.own) {
    gtext_allocator_free(&a, st.done);
    gtext_allocator_free(&a, st.busy);
    gtext_allocator_free(&a, st.own);
    gtext_css_computed_free(style);
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  GTEXT_CSS_Status status = run_pipeline(&st);
  if (result) {
    result->work = st.work;
    css_sub_report(st.sub, result);
  }
  css_sub_free(st.sub);
  for (size_t i = 0; i < st.nshort; i++) {
    gtext_css_longhand_list_free(st.shorts[i].list);
    for (size_t k = 0; k < st.shorts[i].ncycle; k++) {
      gtext_css_longhand_list_free(st.shorts[i].cycle[k]);
    }
    gtext_allocator_free(&a, st.shorts[i].cycle);
  }
  gtext_allocator_free(&a, st.shorts);
  gtext_allocator_free(&a, st.done);
  gtext_allocator_free(&a, st.busy);
  gtext_allocator_free(&a, st.own);
  if (status != GTEXT_CSS_OK) {
    gtext_css_computed_free(style);
    return result_done(result, status);
  }
  *out = style;
  return result_done(result, GTEXT_CSS_OK);
}

GTEXT_CSS_Status gtext_css_compute_element(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Element_Interface * iface,
    const GTEXT_CSS_Declaration_Source * source, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Computed_Style ** out,
    GTEXT_CSS_Compute_Result * result) {
  result_clear(result);
  if (!context || !iface || !source || !source->declarations || !element || !out) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  GTEXT_CSS_Element_Result er;
  GTEXT_CSS_Element_Callback need[] = {GTEXT_CSS_ELEMENT_CB_INHERITANCE_PARENT};
  GTEXT_CSS_Status st = gtext_css_element_check(iface, need, 1, &er);
  if (st != GTEXT_CSS_OK) {
    if (result) {
      css_copy_name(result->name, sizeof result->name, st == GTEXT_CSS_E_UNSUPPORTED ? "parent" : "");
      result->detail = "the element table needs an inheritance_parent or a parent callback";
    }
    return result_done(result, st);
  }
  GTEXT_Allocator a = context->alloc;
  size_t cap = 16, n = 0;
  GTEXT_CSS_Element_Handle * chain = gtext_allocator_malloc(&a, cap * sizeof *chain);
  if (!chain) {
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  GTEXT_CSS_Element_Handle e = element;
  while (e) {
    if (n == context->options.max_element_depth) {
      gtext_allocator_free(&a, chain);
      if (result) {
        result->depth = n;
      }
      return result_done(result, GTEXT_CSS_E_LIMIT_ELEMENTS);
    }
    if (n == cap) {
      if (cap > SIZE_MAX / 2 / sizeof *chain) {
        gtext_allocator_free(&a, chain);
        return result_done(result, GTEXT_CSS_E_OOM);
      }
      GTEXT_CSS_Element_Handle * grown = gtext_allocator_realloc(&a, chain, cap * 2 * sizeof *chain);
      if (!grown) {
        gtext_allocator_free(&a, chain);
        return result_done(result, GTEXT_CSS_E_OOM);
      }
      chain = grown;
      cap *= 2;
    }
    chain[n++] = e;
    GTEXT_CSS_Element_Handle next = NULL;
    st = gtext_css_element_get_inheritance_parent(iface, e, &next, NULL);
    if (st != GTEXT_CSS_OK) {
      gtext_allocator_free(&a, chain);
      return result_done(result, st);
    }
    e = next;
  }
  GTEXT_CSS_Computed_Style * prev = NULL;
  size_t work = 0;
  for (size_t i = n; i-- > 0;) {
    const GTEXT_CSS_Declaration_Block * block = NULL;
    st = source->declarations(source->user, chain[i], &block);
    GTEXT_CSS_Computed_Style * cur = NULL;
    GTEXT_CSS_Compute_Result r;
    if (st == GTEXT_CSS_OK) {
      st = compute_style_with(context, block, prev, chain[i], iface, &cur, &r);
      work += r.work;
      if (st != GTEXT_CSS_OK && result) {
        *result = r;
      }
    }
    gtext_css_computed_free(prev);
    prev = cur;
    if (st != GTEXT_CSS_OK) {
      gtext_allocator_free(&a, chain);
      if (result) {
        result->depth = n - 1 - i;
        result->work = work;
      }
      return result_done(result, st);
    }
  }
  gtext_allocator_free(&a, chain);
  if (result) {
    result->work = work;
    result->depth = n;
  }
  *out = prev;
  return result_done(result, GTEXT_CSS_OK);
}

/* ---- serialization of a style ----------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_computed_serialize(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property,
    const GTEXT_CSS_Serialize_Options * options, const GTEXT_Markup_Sink * sink) {
  const GTEXT_CSS_Value_Set * set = NULL;
  GTEXT_CSS_Status st = gtext_css_computed_value(style, property, &set);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  if (!root) {
    return sink && sink->write ? GTEXT_CSS_OK : GTEXT_CSS_E_INVALID;
  }
  return gtext_css_serialize_value(root, options, sink);
}

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
  bool failed;
} dump_buf;

static GTEXT_Markup_Status dump_write(void * user, const char * data, size_t n) {
  dump_buf * b = user;
  if (b->failed) {
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!grown) {
      b->failed = true;
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, data, n);
  b->len += n;
  b->data[b->len] = '\0';
  return GTEXT_MARKUP_OK;
}

static GTEXT_CSS_Status dump_to_buffer(const GTEXT_CSS_Computed_Style * style, dump_buf * b) {
  memset(b, 0, sizeof *b);
  b->alloc = style->alloc;
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = b;
  sink.write = dump_write;
  dump_write(b, "computed-style 1\n", 17);
  for (size_t i = 0; i < style->n && !b->failed; i++) {
    const GTEXT_CSS_Value_Set * set = style->vals[i];
    if (!set) {
      continue;
    }
    const css_pmeta * m = &style->cc->meta[i];
    dump_write(b, m->name, m->name_length);
    dump_write(b, ": ", 2);
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
    if (root) {
      const GTEXT_CSS_Status st = gtext_css_serialize_value(root, NULL, &sink);
      if (st != GTEXT_CSS_OK) {
        gtext_allocator_free(&b->alloc, b->data);
        return st;
      }
    }
    dump_write(b, "\n", 1);
  }
  for (size_t i = 0; i < gtext_css_computed_custom_count(style) && !b->failed; i++) {
    GTEXT_CSS_String cname;
    const GTEXT_CSS_Custom_Value * cvalue = NULL;
    if (!gtext_css_computed_custom_at(style, i, &cname, &cvalue)) {
      break;
    }
    dump_write(b, cname.data, cname.length);
    dump_write(b, ": ", 2);
    const GTEXT_CSS_Status ws = gtext_css_custom_value_write(cvalue, &sink);
    if (ws != GTEXT_CSS_OK) {
      gtext_allocator_free(&b->alloc, b->data);
      return ws;
    }
    dump_write(b, "\n", 1);
  }
  if (b->failed) {
    gtext_allocator_free(&b->alloc, b->data);
    return GTEXT_CSS_E_OOM;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_computed_dump(const GTEXT_CSS_Computed_Style * style, const GTEXT_Markup_Sink * sink) {
  if (!style || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  dump_buf b;
  GTEXT_CSS_Status st = dump_to_buffer(style, &b);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const GTEXT_Markup_Status rc = sink->write(sink->user, b.data, b.len);
  gtext_allocator_free(&b.alloc, b.data);
  return rc != GTEXT_MARKUP_OK ? GTEXT_CSS_E_WRITE : GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_computed_hash(const GTEXT_CSS_Computed_Style * style, uint64_t * hash) {
  if (!style || !hash) {
    return GTEXT_CSS_E_INVALID;
  }
  dump_buf b;
  GTEXT_CSS_Status st = dump_to_buffer(style, &b);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < b.len; i++) {
    h ^= (unsigned char)b.data[i];
    h *= 0x100000001b3ULL;
  }
  gtext_allocator_free(&b.alloc, b.data);
  *hash = h;
  return GTEXT_CSS_OK;
}
