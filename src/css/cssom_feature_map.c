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
 * CSSFontFeatureValuesMap (CSS Fonts 4, 12.2): the seven value maps of a `@font-feature-values` rule. A map is not an object here: it is a view of the rule, read from the rule's typed
 * model every time (so two "retrieved maps" are the same data by construction, and a map of another rule is not affected), and an edit rewrites the rule as a whole with the
 * machinery of the attribute setters. The blocks of one type are written back as one block holding only the last-defined value of each name, which is how the specification
 * says the rule serializes (13.2); the place of a name that is set again is the place it had.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_typed.h"

static const char * const type_names[GTEXT_CSS_FEATURE_TYPE_COUNT] = {"stylistic", "historical-forms", "styleset", "character-variant", "swash", "ornaments", "annotation"};
static const char * const idl_names[GTEXT_CSS_FEATURE_TYPE_COUNT] = {"stylistic", "historicalForms", "styleset", "characterVariant", "swash", "ornaments", "annotation"};

/// The most values a name of the type may have ("multi-valued feature value definitions"): one, except two for `character-variant` and no limit for `styleset`.
static size_t most_values(GTEXT_CSS_Feature_Type type) {
  return type == GTEXT_CSS_FEATURE_STYLESET ? (size_t)-1 : type == GTEXT_CSS_FEATURE_CHARACTER_VARIANT ? 2 : 1;
}

bool gtext_cssom_feature_map_type(const char * name, size_t length, GTEXT_CSS_Feature_Type * type) {
  if (!name) {
    return false;
  }
  for (int t = 0; t < GTEXT_CSS_FEATURE_TYPE_COUNT; t++) {
    if (strlen(idl_names[t]) == length && memcmp(idl_names[t], name, length) == 0) {
      if (type) {
        *type = (GTEXT_CSS_Feature_Type)t;
      }
      return true;
    }
  }
  return false;
}

/// One entry of a map as the rule reads now: a name and its integers, with the storage they point into.
typedef struct {
  const char * name;
  size_t name_length;
  const uint32_t * values;
  size_t value_count;
} entry;

typedef struct {
  GTEXT_CSS_Atrule * atrule;
  GTEXT_CSS_Feature_Values * model;
  entry * entries[GTEXT_CSS_FEATURE_TYPE_COUNT];   /* deduplicated: a later name replaces the earlier in its place */
  size_t counts[GTEXT_CSS_FEATURE_TYPE_COUNT];
  css_core * core;
} map_view;

static void view_release(map_view * v) {
  for (int t = 0; t < GTEXT_CSS_FEATURE_TYPE_COUNT; t++) {
    gtext_allocator_free(&v->core->alloc, v->entries[t]);
    v->entries[t] = NULL;
  }
  gtext_css_feature_values_free(v->model);
  gtext_css_atrule_free(v->atrule);
  v->model = NULL;
  v->atrule = NULL;
}

static GTEXT_CSS_Status view_of(css_rule * rule, map_view * v) {
  memset(v, 0, sizeof *v);
  v->core = rule->obj.core;
  GTEXT_CSS_Status st = css_rule_typed(v->core, rule, false, &v->atrule);
  if (st == GTEXT_CSS_OK && (!v->atrule || gtext_css_feature_values_from_atrule(v->atrule, &v->model) != GTEXT_CSS_OK)) {
    st = GTEXT_CSS_E_OOM;
  }
  for (int t = 0; st == GTEXT_CSS_OK && t < GTEXT_CSS_FEATURE_TYPE_COUNT; t++) {
    const size_t n = v->model->value_count[t];
    if (!n) {
      continue;
    }
    entry * list = gtext_allocator_calloc(&v->core->alloc, n, sizeof *list);
    if (!list) {
      st = GTEXT_CSS_E_OOM;
      break;
    }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
      const GTEXT_CSS_Feature_Value * f = &v->model->values[t][i];
      size_t at = m;
      for (size_t j = 0; j < m; j++) {
        if (list[j].name_length == f->name.length && memcmp(list[j].name, f->name.data, f->name.length) == 0) {
          at = j;
          break;
        }
      }
      list[at].name = f->name.data;
      list[at].name_length = f->name.length;
      list[at].values = f->values;
      list[at].value_count = f->value_count;
      if (at == m) {
        m++;
      }
    }
    v->entries[t] = list;
    v->counts[t] = m;
  }
  if (st != GTEXT_CSS_OK) {
    view_release(v);
  }
  return st;
}

static GTEXT_CSS_Status enter(GTEXT_CSSOM_Rule * rule, GTEXT_CSS_Feature_Type type, GTEXT_CSSOM_Outcome * outcome, bool writing) {
  if (!rule || (unsigned)type >= (unsigned)GTEXT_CSS_FEATURE_TYPE_COUNT) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_FONT_FEATURE_VALUES) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  if (writing) {
    css_outcome_reset(outcome, rule->obj.core);
    return css_cssom_enter(rule->obj.core);
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_feature_map_size(GTEXT_CSSOM_Rule * rule, GTEXT_CSS_Feature_Type type, size_t * size) {
  GTEXT_CSS_Status st = enter(rule, type, NULL, false);
  if (st != GTEXT_CSS_OK || !size) {
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_INVALID;
  }
  map_view v;
  st = view_of(rule, &v);
  if (st == GTEXT_CSS_OK) {
    *size = v.counts[type];
    view_release(&v);
  }
  return st;
}

/// Copy an entry into one block the caller frees with gtext_cssom_buffer_free(): the integers first (aligned), then the name.
static GTEXT_CSS_Status give(css_core * core, const entry * e, GTEXT_CSSOM_Feature_Entry * out) {
  const size_t vbytes = e->value_count * sizeof(uint32_t);
  char * block = gtext_allocator_malloc(&core->alloc, (vbytes + e->name_length + 1) ? vbytes + e->name_length + 1 : 1);
  if (!block) {
    return GTEXT_CSS_E_OOM;
  }
  if (vbytes) {
    memcpy(block, e->values, vbytes);
  }
  memcpy(block + vbytes, e->name, e->name_length);
  block[vbytes + e->name_length] = '\0';
  out->values = (const uint32_t *)(const void *)block;
  out->value_count = e->value_count;
  out->name = block + vbytes;
  out->name_length = e->name_length;
  out->block = block;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_feature_map_at(GTEXT_CSSOM_Rule * rule, GTEXT_CSS_Feature_Type type, size_t index, GTEXT_CSSOM_Feature_Entry * out) {
  GTEXT_CSS_Status st = enter(rule, type, NULL, false);
  if (st != GTEXT_CSS_OK || !out) {
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_INVALID;
  }
  memset(out, 0, sizeof *out);
  map_view v;
  st = view_of(rule, &v);
  if (st == GTEXT_CSS_OK) {
    st = index < v.counts[type] ? give(v.core, &v.entries[type][index], out) : GTEXT_CSS_E_INDEX_SIZE;
    view_release(&v);
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_feature_map_get(GTEXT_CSSOM_Rule * rule, GTEXT_CSS_Feature_Type type, const char * name, size_t name_length, GTEXT_CSSOM_Feature_Entry * out, bool * found) {
  GTEXT_CSS_Status st = enter(rule, type, NULL, false);
  if (st != GTEXT_CSS_OK || !out || !found || (!name && name_length)) {
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_INVALID;
  }
  memset(out, 0, sizeof *out);
  *found = false;
  map_view v;
  st = view_of(rule, &v);
  if (st == GTEXT_CSS_OK) {
    for (size_t i = 0; st == GTEXT_CSS_OK && i < v.counts[type]; i++) {
      const entry * e = &v.entries[type][i];
      if (e->name_length == name_length && (!name_length || memcmp(e->name, name, name_length) == 0)) {
        st = give(v.core, e, out);
        *found = st == GTEXT_CSS_OK;
        break;
      }
    }
    view_release(&v);
  }
  return st;
}

/* ---- writing ---------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_Markup_Status name_write(void * user, const char * bytes, size_t n) {
  return tb_put((tbuf *)user, bytes, n) ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_OOM;
}

/// The edit being made while the body is written: `type`'s `name` gets `values` (set), is dropped (`drop`), or the whole map is dropped (`clear`).
typedef struct {
  GTEXT_CSS_Feature_Type type;
  const char * name;
  size_t name_length;
  const uint32_t * values;
  size_t value_count;
  bool set;
  bool drop;
  bool clear;
} edit;

static GTEXT_CSS_Status write_value_list(tbuf * out, const uint32_t * values, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char digits[16];
    int len = 0;
    uint32_t x = values[i];
    do {
      digits[len++] = (char)('0' + x % 10);
      x /= 10;
    } while (x);
    if (i && !tb_puts(out, " ")) {
      return GTEXT_CSS_E_OOM;
    }
    while (len--) {
      if (!tb_put(out, &digits[len], 1)) {
        return GTEXT_CSS_E_OOM;
      }
    }
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status write_name(tbuf * out, const char * name, size_t length) {
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = out;
  sink.write = name_write;
  return css_serialize_identifier_text(name, length, false, &sink);
}

static GTEXT_CSS_Status write_block(tbuf * out, map_view * v, int t, const edit * e) {
  /* the entries of the type as the edit leaves them, in order */
  tbuf body;
  tb_init(&body, &v->core->alloc);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  size_t written = 0;
  bool placed = false;
  const bool mine = e && (int)e->type == t;
  for (size_t i = 0; st == GTEXT_CSS_OK && i < v->counts[t] && !(mine && e->clear); i++) {
    const entry * en = &v->entries[t][i];
    const bool same = mine && en->name_length == e->name_length && (!e->name_length || memcmp(en->name, e->name, e->name_length) == 0);
    if (same && e->drop) {
      continue;
    }
    st = write_name(&body, en->name, en->name_length);
    if (st == GTEXT_CSS_OK && (!tb_puts(&body, ": "))) {
      st = GTEXT_CSS_E_OOM;
    }
    if (st == GTEXT_CSS_OK) {
      st = same && e->set ? write_value_list(&body, e->values, e->value_count) : write_value_list(&body, en->values, en->value_count);
    }
    if (st == GTEXT_CSS_OK && !tb_puts(&body, "; ")) {
      st = GTEXT_CSS_E_OOM;
    }
    placed = placed || same;
    written++;
  }
  if (st == GTEXT_CSS_OK && mine && e->set && !placed) {
    st = write_name(&body, e->name, e->name_length);
    if (st == GTEXT_CSS_OK && !tb_puts(&body, ": ")) {
      st = GTEXT_CSS_E_OOM;
    }
    if (st == GTEXT_CSS_OK) {
      st = write_value_list(&body, e->values, e->value_count);
    }
    if (st == GTEXT_CSS_OK && !tb_puts(&body, "; ")) {
      st = GTEXT_CSS_E_OOM;
    }
    written++;
  }
  if (st == GTEXT_CSS_OK && written && (!tb_puts(out, "@") || !tb_puts(out, type_names[t]) || !tb_puts(out, " { ") || !tb_put(out, body.data, body.len) || !tb_puts(out, "} "))) {
    st = GTEXT_CSS_E_OOM;
  }
  if (body.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  tb_free(&body);
  return st;
}

/// The body of the rule: its descriptors (`font-display`), then the blocks, the edit applied.
static GTEXT_CSS_Status write_body(css_rule * rule, map_view * v, const edit * e, tbuf * out) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const size_t nd = gtext_css_atrule_descriptor_count(v->atrule);
  for (size_t i = 0; i < nd && st == GTEXT_CSS_OK; i++) {
    GTEXT_CSS_Atrule_Descriptor d;
    gtext_css_atrule_descriptor_at(v->atrule, i, &d);
    if (!tb_put(out, d.name, d.name_length) || !tb_puts(out, ": ") || !tb_put(out, d.text.data, d.text.length) || !tb_puts(out, d.important ? " !important; " : "; ")) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  for (int t = 0; t < GTEXT_CSS_FEATURE_TYPE_COUNT && st == GTEXT_CSS_OK; t++) {
    st = write_block(out, v, t, e);
  }
  (void)rule;
  return st;
}

GTEXT_CSS_Status css_feature_values_body(css_rule * rule, tbuf * out) {
  map_view v;
  GTEXT_CSS_Status st = view_of(rule, &v);
  if (st == GTEXT_CSS_OK) {
    st = write_body(rule, &v, NULL, out);
    view_release(&v);
  }
  return st;
}

/// The rule as read again holds what was set (a name an identifier cannot carry, and a value list the model reads differently, are not kept).
static GTEXT_CSS_Status check_edit(css_rule * rule, const css_own * own, void * user, bool * ok) {
  const edit * e = user;
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = css_own_typed(rule, own, false, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Feature_Values * model = NULL;
  st = a ? gtext_css_feature_values_from_atrule(a, &model) : GTEXT_CSS_E_OOM;
  *ok = true;
  if (st == GTEXT_CSS_OK && e->set) {
    bool found = false;
    for (size_t i = 0; i < model->value_count[e->type]; i++) {
      const GTEXT_CSS_Feature_Value * f = &model->values[e->type][i];
      if (f->name.length == e->name_length && (!e->name_length || memcmp(f->name.data, e->name, e->name_length) == 0)) {
        found = f->value_count == e->value_count && (!e->value_count || memcmp(f->values, e->values, e->value_count * sizeof *f->values) == 0);
      }
    }
    *ok = found;
  }
  gtext_css_feature_values_free(model);
  gtext_css_atrule_free(a);
  return st;
}

static GTEXT_CSS_Status rewrite(css_rule * rule, map_view * v, const edit * e, GTEXT_CSSOM_Outcome * outcome) {
  tbuf header, body;
  tb_init(&header, &v->core->alloc);
  tb_init(&body, &v->core->alloc);
  GTEXT_CSS_Status st = css_rule_header_text(rule, &header);
  if (st == GTEXT_CSS_OK) {
    st = write_body(rule, v, e, &body);
  }
  bool ignored = false;
  if (st == GTEXT_CSS_OK && (header.oom || body.oom)) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK) {
    st = css_rule_replace_own(rule, header.data, header.len, body.data, body.len, false, check_edit, (void *)e, &ignored, outcome);
  }
  tb_free(&header);
  tb_free(&body);
  if (st == GTEXT_CSS_OK && ignored) {
    outcome->kind = GTEXT_CSSOM_IGNORED_INVALID_VALUE;
    outcome->detail = "the rule does not read back with that name: nothing changes";
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_feature_map_set(GTEXT_CSSOM_Rule * rule, GTEXT_CSS_Feature_Type type, const char * name, size_t name_length, const uint32_t * values, size_t value_count,
    GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (rule && !outcome) {
    memset(&local, 0, sizeof local);
  }
  GTEXT_CSS_Status st = enter(rule, type, out, true);
  if (st != GTEXT_CSS_OK || (!name && name_length) || (!values && value_count)) {
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_INVALID;
  }
  if (value_count == 0 || value_count > most_values(type)) {
    return GTEXT_CSS_E_INVALID_ACCESS;
  }
  if (name_length > rule->obj.core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  map_view v;
  st = view_of(rule, &v);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  for (size_t i = 0; i < v.counts[type]; i++) {
    const entry * en = &v.entries[type][i];
    if (en->name_length == name_length && (!name_length || memcmp(en->name, name, name_length) == 0) && en->value_count == value_count &&
        memcmp(en->values, values, value_count * sizeof *values) == 0) {
      view_release(&v);
      return GTEXT_CSS_OK;   /* says what the map says: not a change */
    }
  }
  const edit e = {type, name, name_length, values, value_count, true, false, false};
  st = rewrite(rule, &v, &e, out);
  view_release(&v);
  return st;
}

GTEXT_CSS_Status gtext_cssom_feature_map_delete(GTEXT_CSSOM_Rule * rule, GTEXT_CSS_Feature_Type type, const char * name, size_t name_length, bool * deleted, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (rule && !outcome) {
    memset(&local, 0, sizeof local);
  }
  GTEXT_CSS_Status st = enter(rule, type, out, true);
  if (deleted) {
    *deleted = false;
  }
  if (st != GTEXT_CSS_OK || (!name && name_length)) {
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_INVALID;
  }
  map_view v;
  st = view_of(rule, &v);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  bool present_now = false;
  for (size_t i = 0; i < v.counts[type]; i++) {
    const entry * en = &v.entries[type][i];
    present_now = present_now || (en->name_length == name_length && (!name_length || memcmp(en->name, name, name_length) == 0));
  }
  if (present_now) {
    const edit e = {type, name, name_length, NULL, 0, false, true, false};
    st = rewrite(rule, &v, &e, out);
    if (st == GTEXT_CSS_OK && deleted) {
      *deleted = out->kind == GTEXT_CSSOM_CHANGED;
    }
  }
  view_release(&v);
  return st;
}

GTEXT_CSS_Status gtext_cssom_feature_map_clear(GTEXT_CSSOM_Rule * rule, GTEXT_CSS_Feature_Type type, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (rule && !outcome) {
    memset(&local, 0, sizeof local);
  }
  GTEXT_CSS_Status st = enter(rule, type, out, true);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  map_view v;
  st = view_of(rule, &v);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (v.counts[type]) {
    const edit e = {type, NULL, 0, NULL, 0, false, false, true};
    st = rewrite(rule, &v, &e, out);
  }
  view_release(&v);
  return st;
}
