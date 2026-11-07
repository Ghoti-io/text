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
 * What CSSOM says of the rules of CSS Functions and Mixins 1 beside their text (cssom_rule_serialize.c writes that): `CSSMixinRule.name`, `.getParameters()` and `.contents`, and
 * `CSSApplyBlockRule.name` and `.getArguments()` (the statement form has the same two). They are read from the rule's syntax node each time, as the model of the rest of CSSOM's
 * rules is: a name and the prelude's parameters and arguments never change after the rule is made, and what a mixin holds does (so `contents` walks the rules inside it).
 */

#include <string.h>

#include "css_fndef.h"
#include "css_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_rule_internal.h"
#include "cssom_typed.h"

static GTEXT_Markup_Status ident_write(void * user, const char * bytes, size_t n) {
  return tb_put((tbuf *)user, bytes, n) ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_OOM;
}

/// "serialize an identifier", into `out`.
static GTEXT_CSS_Status put_ident(tbuf * out, const char * name, size_t length) {
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = out;
  sink.write = ident_write;
  const GTEXT_CSS_Status st = css_serialize_identifier_text(name, length, false, &sink);
  return st == GTEXT_CSS_E_WRITE ? GTEXT_CSS_E_OOM : st;   /* the sink is a buffer of ours: one that could not take the bytes says E_WRITE, and here that is memory */
}

/// Whether a `@contents` rule is anywhere inside the rule: "True if the mixin accepts a contents block". An explicit stack, so that a nest of any depth is read with no recursion.
static GTEXT_CSS_Status holds_contents(css_core * core, css_rule * mixin, bool * found) {
  GTEXT_Allocator a = core->alloc;
  css_rule ** stack = NULL;
  size_t sp = 0, cap = 0;
  css_rule * cur = css_rule_list_first(&mixin->rules);
  *found = false;
  for (;;) {
    while (cur && !*found) {
      if (cur->kind == GTEXT_CSSOM_RULE_CONTENTS) {
        *found = true;
        break;
      }
      css_rule * next = css_rule_list_next(cur);
      if (cur->rules.count > 0) {
        if (sp == cap) {
          css_rule ** grown = gtext_allocator_realloc(&a, stack, (cap ? cap * 2 : 16) * sizeof *grown);
          if (!grown) {
            gtext_allocator_free(&a, stack);
            return GTEXT_CSS_E_OOM;
          }
          stack = grown;
          cap = cap ? cap * 2 : 16;
        }
        stack[sp++] = next;
        next = css_rule_list_first(&cur->rules);
      }
      cur = next;
    }
    if (*found || sp == 0) {
      break;
    }
    cur = stack[--sp];
  }
  gtext_allocator_free(&a, stack);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_mixin_attribute(css_rule * rule, css_mixin_attr which, tbuf * out) {
  css_core * core = rule->obj.core;
  if (which == CSS_MIXIN_ATTR_CONTENTS) {
    bool found = false;
    const GTEXT_CSS_Status st = holds_contents(core, rule, &found);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    return (found ? tb_puts(out, "true") : tb_puts(out, "false")) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (rule->kind == GTEXT_CSSOM_RULE_MIXIN) {
    css_fn_def * def = NULL;
    bool valid = false;
    GTEXT_CSS_Status st = css_mixin_prelude_read(core->registry, &core->alloc, rule->node, &def, &valid);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (!valid || !def) {
      return GTEXT_CSS_E_INTERNAL;
    }
    st = put_ident(out, def->name, def->name_len);
    css_fn_def_free(def);
    return st;
  }
  css_apply_prelude ap;
  bool valid = false;
  GTEXT_CSS_Status st = css_apply_prelude_read(&core->alloc, rule->node, &ap, &valid);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!valid) {
    return GTEXT_CSS_E_INTERNAL;
  }
  st = put_ident(out, ap.name.data, ap.name.length);
  css_apply_prelude_free(&core->alloc, &ap);
  return st;
}

/* ---- getParameters() and getArguments() ----------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_mixin_get_parameters(GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Function_Parameters * out) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_MIXIN) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  css_core * core = rule->obj.core;
  css_fn_def * def = NULL;
  bool valid = false;
  GTEXT_CSS_Status st = css_mixin_prelude_read(core->registry, &core->alloc, rule->node, &def, &valid);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!valid || !def) {
    return GTEXT_CSS_E_INTERNAL;
  }
  /* each parameter's three strings are written one after the other into the text that follows the array of parameters: the name as an identifier is the name as written, the type is the syntax string
     or `*` for none ("or "*" if the parameter has no type"), the default is the text it was written as, trimmed ("serialize a CSS value") */
  tbuf text;
  tb_init(&text, &core->alloc);
  size_t * at = def->nparams ? gtext_allocator_calloc(&core->alloc, def->nparams * 3 * 2, sizeof *at) : NULL;
  if (def->nparams && !at) {
    css_fn_def_free(def);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < def->nparams && st == GTEXT_CSS_OK; i++) {
    const css_fn_param * p = &def->params[i];
    size_t * e = &at[i * 6];
    e[0] = text.len;
    st = put_ident(&text, p->name, p->name_len);
    e[1] = text.len - e[0];
    e[2] = text.len;
    const char * type = NULL;
    size_t tl = 0;
    bool wrapped = false, universal = false;
    if (st == GTEXT_CSS_OK) {
      if (css_fn_param_css_type(p, &type, &tl, &wrapped, &universal)) {
        if (!tb_put(&text, type, tl)) {
          st = GTEXT_CSS_E_OOM;
        }
      }
      else if (!tb_puts(&text, "*")) {
        st = GTEXT_CSS_E_OOM;
      }
    }
    e[3] = text.len - e[2];
    e[4] = text.len;
    if (st == GTEXT_CSS_OK && p->has_default && !tb_put(&text, p->default_text, p->default_len)) {
      st = GTEXT_CSS_E_OOM;
    }
    e[5] = text.len - e[4];
  }
  if (st == GTEXT_CSS_OK && text.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  const size_t n = def->nparams;
  if (st != GTEXT_CSS_OK) {
    tb_free(&text);
    gtext_allocator_free(&core->alloc, at);
    css_fn_def_free(def);
    return st;
  }
  const size_t head = n * sizeof(GTEXT_CSSOM_Function_Parameter);
  char * block = gtext_allocator_malloc(&core->alloc, head + text.len + 1);
  if (!block) {
    tb_free(&text);
    gtext_allocator_free(&core->alloc, at);
    css_fn_def_free(def);
    return GTEXT_CSS_E_OOM;
  }
  char * strings = block + head;
  if (text.len) {
    memcpy(strings, text.data, text.len);
  }
  strings[text.len] = '\0';
  GTEXT_CSSOM_Function_Parameter * items = (GTEXT_CSSOM_Function_Parameter *)(void *)block;
  for (size_t i = 0; i < n; i++) {
    const size_t * e = &at[i * 6];
    items[i].name = strings + e[0];
    items[i].name_length = e[1];
    items[i].type = strings + e[2];
    items[i].type_length = e[3];
    items[i].has_default = def->params[i].has_default;
    items[i].default_value = strings + e[4];
    items[i].default_length = e[5];
  }
  out->items = n ? items : NULL;
  out->count = n;
  out->block = block;
  tb_free(&text);
  gtext_allocator_free(&core->alloc, at);
  css_fn_def_free(def);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_apply_get_arguments(GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_String_List * out) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_APPLY) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  css_core * core = rule->obj.core;
  css_apply_prelude ap;
  bool valid = false;
  GTEXT_CSS_Status st = css_apply_prelude_read(&core->alloc, rule->node, &ap, &valid);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!valid) {
    return GTEXT_CSS_E_INTERNAL;
  }
  /* "Returns a sequence of arguments as strings, each item serialized as if it had been the specified value of a custom property": an argument written as `{}` is its contents */
  const size_t n = ap.nargs;
  tbuf text;
  tb_init(&text, &core->alloc);
  size_t * at = n ? gtext_allocator_calloc(&core->alloc, n * 2, sizeof *at) : NULL;
  if (n && !at) {
    css_apply_prelude_free(&core->alloc, &ap);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < n && st == GTEXT_CSS_OK; i++) {
    at[i * 2] = text.len;
    if (ap.args[i].count) {
      GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
      so.allocator = &core->alloc;
      char * s = NULL;
      size_t sl = 0;
      st = gtext_css_serialize_values_buffer(ap.args[i].values, ap.args[i].count, &so, &s, &sl);
      if (st == GTEXT_CSS_OK) {
        if (!tb_put(&text, s, sl)) {
          st = GTEXT_CSS_E_OOM;
        }
        gtext_css_serialize_buffer_free(&so, s);
      }
    }
    at[i * 2 + 1] = text.len - at[i * 2];
  }
  if (st == GTEXT_CSS_OK && text.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  const size_t head = n * sizeof(GTEXT_CSSOM_String_Item);
  char * block = st == GTEXT_CSS_OK ? gtext_allocator_malloc(&core->alloc, head + text.len + 1) : NULL;
  if (st == GTEXT_CSS_OK && !block) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK) {
    char * strings = block + head;
    if (text.len) {
      memcpy(strings, text.data, text.len);
    }
    strings[text.len] = '\0';
    GTEXT_CSSOM_String_Item * items = (GTEXT_CSSOM_String_Item *)(void *)block;
    for (size_t i = 0; i < n; i++) {
      items[i].text = strings + at[i * 2];
      items[i].length = at[i * 2 + 1];
    }
    out->items = n ? items : NULL;
    out->count = n;
    out->block = block;
  }
  tb_free(&text);
  gtext_allocator_free(&core->alloc, at);
  css_apply_prelude_free(&core->alloc, &ap);
  return st;
}
