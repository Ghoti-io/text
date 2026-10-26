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

#include "css_registry_internal.h"

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
static GTEXT_CSS_Status run_match(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Compiled * compiled,
    const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Property_Options * options, bool unitless, bool name_is_clip,
    GTEXT_CSS_Property_Value * out) {
  GTEXT_CSS_Match_Options mo = options && options->match ? *options->match : *css_registry_match_options(registry);
  mo.allocator = options && options->allocator ? options->allocator : css_registry_allocator(registry);
  mo.unitless_length = unitless;
  mo.unitless_length_nested = unitless && name_is_clip;
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

/**
 * Whether every `var()`, `env()` and `attr()` in the values is syntactically valid. Values 5, on the arbitrary substitution functions:
 * "If a property value contains one or more arbitrary substitution functions, and all of those functions are themselves syntactically
 * valid according to their argument grammars, the entire value's grammar must be assumed to be valid at parse time". The argument
 * grammar of the three is `var( <declaration-value> , <declaration-value>? )` (Variables 1, 3; `<env-args>` and `<attr-args>` the
 * same): the first argument, up to the first top-level comma, has to hold something, so `var()` and `var(, x)` are not valid.
 * What the first argument names (a `--` name, an environment variable) is checked when the value is substituted, not now.
 * Walks with its own stack, so nesting costs memory and not stack.
 */
static GTEXT_CSS_Status substitution_syntax(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_Allocator * a, bool * valid) {
  *valid = true;
  struct frame {
    const GTEXT_CSS_Component_Value * v;
    size_t n;
  };
  size_t cap = 16, top = 0;
  struct frame * st = gtext_allocator_malloc(a, cap * sizeof *st);
  if (!st) {
    return GTEXT_CSS_E_OOM;
  }
  st[top++] = (struct frame){values, count};
  while (top > 0 && *valid) {
    struct frame f = st[--top];
    for (size_t i = 0; i < f.n && *valid; i++) {
      const GTEXT_CSS_Component_Value * v = &f.v[i];
      if (v->kind == GTEXT_CSS_VALUE_FUNCTION && (name_is(v, "var") || name_is(v, "env") || name_is(v, "attr"))) {
        bool first = false;
        for (size_t k = 0; k < v->child_count; k++) {
          const GTEXT_CSS_Component_Value * c = &v->children[k];
          if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_COMMA) {
            break;
          }
          if (!(c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_WHITESPACE)) {
            first = true;
            break;
          }
        }
        if (!first) {
          *valid = false;
        }
      }
      if (v->child_count > 0) {
        if (top == cap) {
          cap *= 2;
          struct frame * g = gtext_allocator_realloc(a, st, cap * sizeof *st);
          if (!g) {
            gtext_allocator_free(a, st);
            return GTEXT_CSS_E_OOM;
          }
          st = g;
        }
        st[top++] = (struct frame){v->children, v->child_count};
      }
    }
  }
  gtext_allocator_free(a, st);
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status parse_pending(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Value * out) {
  const GTEXT_CSS_Compiled * pending = NULL;
  GTEXT_CSS_Status st = css_registry_pending_grammar(registry, &pending);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = run_match(registry, pending, values, count, options, false, false, out);
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
  return run_match(registry, compiled, values, count, options, false, false, out);
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
  /* `all` takes nothing else */
  if (is_all(&info)) {
    out->outcome = GTEXT_CSS_PROP_RESULT_INVALID;
    return GTEXT_CSS_OK;
  }
  bool sub = false;
  st = gtext_css_has_arbitrary_substitution(values, count,
      options->allocator ? options->allocator : css_registry_allocator(registry), &sub);
  if (st != GTEXT_CSS_OK) {
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    return st;
  }
  if (sub) {
    bool syntax = true;
    st = substitution_syntax(values, count, options->allocator ? options->allocator : css_registry_allocator(registry), &syntax);
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
  st = css_registry_property_compiled(registry, target, &state, &compiled, NULL);
  if (st != GTEXT_CSS_OK) {
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    return st;
  }
  if (state != GTEXT_CSS_GRAMMAR_OK) {
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  st = run_match(registry, compiled, values, count, options, options->quirks && (info.flags & GTEXT_CSS_PROP_QUIRK_UNITLESS) != 0, info.name_length == 4 && memcmp(info.name, "clip", 4) == 0, out);
  if (st != GTEXT_CSS_OK) {
    gtext_css_property_value_free(out);
    out->outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  }
  return st;
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
  st = gtext_css_parse_property(registry, name, name_length, tree->values, tree->value_count, options, out);
  gtext_css_tree_free(tree);
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
  return gtext_css_parse_property(registry, declaration->name.data, declaration->name.length, declaration->value,
      declaration->value_count, options, out);
}

/* ---- serialization ------------------------------------------------------------------------ */

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
