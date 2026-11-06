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
 * `@function` rules (css_custom_function.h): the prelude read into a definition, the body looked through for what it may hold, and the order of definitions of one name.
 */

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_custom_function.h>
#include <ghoti.io/text/css/css_layer.h>

#include <string.h>

#include "css_creg.h"
#include "css_fndef.h"
#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_syntax_string.h"

#define DEFAULT_MAX_FUNCTIONS ((size_t)4096)
#define LIMIT_MAX_FUNCTIONS ((size_t)1 << 24)
#define DEFAULT_MAX_PARAMETERS ((size_t)1024)
#define LIMIT_MAX_PARAMETERS ((size_t)1 << 20)
#define DEFAULT_MAX_DIAGNOSTICS ((size_t)1024)
#define LIMIT_MAX_DIAGNOSTICS ((size_t)1 << 20)
#define DEFAULT_RULE_DEPTH ((size_t)4096)
#define LIMIT_RULE_DEPTH ((size_t)1 << 20)

typedef struct {
  GTEXT_CSS_Function_Reason reason;
  char * name;
  size_t name_len;
  GTEXT_CSS_Id rule;
  GTEXT_CSS_Span span;
  size_t sheet;
  const char * message;
} diag_rec;

struct GTEXT_CSS_Function_Set {
  GTEXT_Allocator alloc;
  const GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Function_Options opt;
  css_fn_def ** defs;          ///< Sorted by the bytes of the name.
  size_t ndefs, defcap;
  diag_rec * diags;
  size_t ndiags, diagcap;
  size_t total;
  size_t sheets;
  size_t work;
  GTEXT_CSS_Layer_Registry * layers;
  size_t layer_work;
};

GTEXT_CSS_Function_Options gtext_css_function_options_default(void) {
  GTEXT_CSS_Function_Options o;
  memset(&o, 0, sizeof o);
  o.max_functions = DEFAULT_MAX_FUNCTIONS;
  o.max_parameters = DEFAULT_MAX_PARAMETERS;
  o.max_diagnostics = DEFAULT_MAX_DIAGNOSTICS;
  o.max_rule_depth = DEFAULT_RULE_DEPTH;
  return o;
}

static size_t clamp(size_t v, size_t dflt, size_t max) {
  return v == 0 ? dflt : (v > max ? max : v);
}

GTEXT_CSS_Status gtext_css_function_set_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Function_Options * options, GTEXT_CSS_Function_Set ** out) {
  if (!registry || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * ap = options && options->allocator ? options->allocator : css_registry_allocator(registry);
  GTEXT_Allocator a = *ap;
  GTEXT_CSS_Function_Set * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  s->alloc = a;
  s->registry = registry;
  if (options) {
    s->opt = *options;
  }
  s->opt.allocator = NULL;
  s->opt.max_functions = clamp(s->opt.max_functions, DEFAULT_MAX_FUNCTIONS, LIMIT_MAX_FUNCTIONS);
  s->opt.max_parameters = clamp(s->opt.max_parameters, DEFAULT_MAX_PARAMETERS, LIMIT_MAX_PARAMETERS);
  s->opt.max_diagnostics = clamp(s->opt.max_diagnostics, DEFAULT_MAX_DIAGNOSTICS, LIMIT_MAX_DIAGNOSTICS);
  s->opt.max_rule_depth = clamp(s->opt.max_rule_depth, DEFAULT_RULE_DEPTH, LIMIT_RULE_DEPTH);
  *out = s;
  return GTEXT_CSS_OK;
}

void css_fn_def_free(css_fn_def * d) {
  if (!d) {
    return;
  }
  GTEXT_Allocator a = d->alloc;
  for (size_t i = 0; i < d->nparams; i++) {
    gtext_allocator_free(&a, d->params[i].name);
    css_compiled_syntax_free(d->params[i].type);
    gtext_allocator_free(&a, d->params[i].default_text);
  }
  gtext_allocator_free(&a, d->params);
  css_compiled_syntax_free(d->returns);
  gtext_allocator_free(&a, d->name);
  gtext_allocator_free(&a, d);
}

void gtext_css_function_set_free(GTEXT_CSS_Function_Set * s) {
  if (!s) {
    return;
  }
  GTEXT_Allocator a = s->alloc;
  for (size_t i = 0; i < s->ndefs; i++) {
    css_fn_def_free(s->defs[i]);
  }
  gtext_allocator_free(&a, s->defs);
  for (size_t i = 0; i < s->ndiags; i++) {
    gtext_allocator_free(&a, s->diags[i].name);
  }
  gtext_allocator_free(&a, s->diags);
  gtext_css_layer_registry_free(s->layers);
  gtext_allocator_free(&a, s);
}

const char * gtext_css_function_reason_name(GTEXT_CSS_Function_Reason reason) {
  switch (reason) {
  case GTEXT_CSS_FN_BAD_PRELUDE: return "bad-prelude";
  case GTEXT_CSS_FN_BAD_NAME: return "bad-name";
  case GTEXT_CSS_FN_BAD_PARAMETER: return "bad-parameter";
  case GTEXT_CSS_FN_DUPLICATE_PARAMETER: return "duplicate-parameter";
  case GTEXT_CSS_FN_BAD_TYPE: return "bad-type";
  case GTEXT_CSS_FN_BAD_DEFAULT: return "bad-default";
  case GTEXT_CSS_FN_BAD_RETURNS: return "bad-returns";
  case GTEXT_CSS_FN_UNKNOWN_DESCRIPTOR: return "unknown-descriptor";
  case GTEXT_CSS_FN_SUPERSEDED: return "superseded";
  default: return "unknown";
  }
}

static GTEXT_CSS_Status diagnose(GTEXT_CSS_Function_Set * s, GTEXT_CSS_Function_Reason reason, const char * name, size_t name_len, const GTEXT_CSS_Rule * rule,
    const GTEXT_CSS_Span * span, const char * message) {
  s->total++;
  if (s->ndiags >= s->opt.max_diagnostics) {
    return GTEXT_CSS_OK;
  }
  if (s->ndiags == s->diagcap) {
    const size_t cap = s->diagcap ? s->diagcap * 2 : 8;
    diag_rec * g = gtext_allocator_realloc(&s->alloc, s->diags, cap * sizeof *g);
    if (!g) {
      return GTEXT_CSS_E_OOM;
    }
    s->diags = g;
    s->diagcap = cap;
  }
  char * copy = gtext_allocator_malloc(&s->alloc, name_len + 1);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  if (name_len) {
    memcpy(copy, name, name_len);
  }
  copy[name_len] = '\0';
  diag_rec * d = &s->diags[s->ndiags++];
  d->reason = reason;
  d->name = copy;
  d->name_len = name_len;
  d->rule = rule->id;
  d->span = *span;
  d->sheet = s->sheets ? s->sheets - 1 : 0;
  d->message = message;
  return GTEXT_CSS_OK;
}

size_t gtext_css_function_diagnostic_total(const GTEXT_CSS_Function_Set * s) {
  return s ? s->total : 0;
}

bool gtext_css_function_diagnostic(const GTEXT_CSS_Function_Set * s, size_t index, GTEXT_CSS_Function_Diagnostic * out) {
  if (!s || !out || index >= s->ndiags) {
    return false;
  }
  const diag_rec * d = &s->diags[index];
  out->reason = d->reason;
  out->name.data = d->name;
  out->name.length = d->name_len;
  out->rule = d->rule;
  out->span = d->span;
  out->sheet = d->sheet;
  out->message = d->message;
  return true;
}

/* ---- the set ------------------------------------------------------------------------------------------ */

size_t gtext_css_function_count(const GTEXT_CSS_Function_Set * s) {
  return s ? s->ndefs : 0;
}

static int name_cmp(const char * a, size_t al, const char * b, size_t bl) {
  const size_t n = al < bl ? al : bl;
  const int c = n ? memcmp(a, b, n) : 0;
  if (c) {
    return c;
  }
  return al < bl ? -1 : al > bl ? 1 : 0;
}

/// The place of a name in the sorted array: its index when there, else where it goes. `*found` says which.
static size_t search(const GTEXT_CSS_Function_Set * s, const char * name, size_t length, bool * found, size_t * work) {
  size_t lo = 0, hi = s->ndefs;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const css_fn_def * d = s->defs[mid];
    if (work) {
      (*work)++;
    }
    const int c = name_cmp(name, length, d->name, d->name_len);
    if (c == 0) {
      *found = true;
      return mid;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  *found = false;
  return lo;
}

const css_fn_def * css_function_set_find(const GTEXT_CSS_Function_Set * s, const char * name, size_t length, size_t * work) {
  if (!s || !name) {
    return NULL;
  }
  bool found = false;
  const size_t at = search(s, name, length, &found, work);
  return found ? s->defs[at] : NULL;
}

static void fill_info(const css_fn_def * d, GTEXT_CSS_Function_Info * info) {
  memset(info, 0, sizeof *info);
  info->name.data = d->name;
  info->name.length = d->name_len;
  info->parameter_count = d->nparams;
  info->has_returns = d->returns != NULL;
  if (d->returns) {
    info->returns.data = d->returns->text;
    info->returns.length = d->returns->len;
  }
  info->has_result = d->has_result;
  info->rule = d->rule;
  info->layer = d->layer;
  info->sheet = d->sheet;
}

bool gtext_css_function_at(const GTEXT_CSS_Function_Set * s, size_t index, GTEXT_CSS_Function_Info * info) {
  if (!s || !info || index >= s->ndefs) {
    return false;
  }
  fill_info(s->defs[index], info);
  return true;
}

bool gtext_css_function_find(const GTEXT_CSS_Function_Set * s, const char * name, size_t length, GTEXT_CSS_Function_Info * info) {
  const css_fn_def * d = css_function_set_find(s, name, length, NULL);
  if (!d || !info) {
    return false;
  }
  fill_info(d, info);
  return true;
}

bool gtext_css_function_parameter(const GTEXT_CSS_Function_Set * s, const char * name, size_t length, size_t index, GTEXT_CSS_Function_Parameter * out) {
  const css_fn_def * d = css_function_set_find(s, name, length, NULL);
  if (!d || !out || index >= d->nparams) {
    return false;
  }
  const css_fn_param * p = &d->params[index];
  memset(out, 0, sizeof *out);
  out->name.data = p->name;
  out->name.length = p->name_len;
  out->has_type = p->type != NULL;
  if (p->type) {
    out->type.data = p->type->text;
    out->type.length = p->type->len;
  }
  out->has_default = p->has_default;
  if (p->has_default) {
    out->default_value.data = p->default_text;
    out->default_value.length = p->default_len;
  }
  return true;
}

/* ---- the prelude -------------------------------------------------------------------------------------- */

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool ieq(const GTEXT_CSS_String * s, const char * lower) {
  return css_ascii_ieq(s, lower);
}

/// The range [*a, *b) of the values without the white space at either end.
static void trim(const GTEXT_CSS_Component_Value * v, size_t * a, size_t * b) {
  while (*a < *b && is_ws(&v[*a])) {
    (*a)++;
  }
  while (*b > *a && is_ws(&v[*b - 1])) {
    (*b)--;
  }
}

/// What reading a piece of the prelude found.
typedef enum {
  PRE_OK = -1,
  PRE_BAD_PRELUDE = GTEXT_CSS_FN_BAD_PRELUDE,
  PRE_BAD_NAME = GTEXT_CSS_FN_BAD_NAME,
  PRE_BAD_PARAMETER = GTEXT_CSS_FN_BAD_PARAMETER,
  PRE_DUPLICATE = GTEXT_CSS_FN_DUPLICATE_PARAMETER,
  PRE_BAD_TYPE = GTEXT_CSS_FN_BAD_TYPE,
  PRE_BAD_DEFAULT = GTEXT_CSS_FN_BAD_DEFAULT,
  PRE_BAD_RETURNS = GTEXT_CSS_FN_BAD_RETURNS
} pre_verdict;

static char * copy_n(const GTEXT_Allocator * a, const char * s, size_t n) {
  char * c = gtext_allocator_malloc((GTEXT_Allocator *)a, n + 1);
  if (!c) {
    return NULL;
  }
  if (n) {
    memcpy(c, s, n);
  }
  c[n] = '\0';
  return c;
}

/// Read a <css-type> from the values: one syntax component (`<length>+`, `auto`), or `type(<syntax>)`. `*type` is the compiled syntax (owned) when `*verdict` is PRE_OK.
/// "<css-type> = <syntax-component> | <type()>": a syntax that needs a combinator or is `*` alone has to be wrapped in `type()`.
static GTEXT_CSS_Status read_type(GTEXT_CSS_Function_Set * s, const GTEXT_CSS_Component_Value * v, size_t a, size_t b, css_compiled_syntax ** type, pre_verdict * verdict) {
  *type = NULL;
  *verdict = PRE_BAD_TYPE;
  trim(v, &a, &b);
  if (a == b) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Component_Value * inner = v + a;
  size_t n = b - a;
  bool wrapped = false;
  if (n == 1 && v[a].kind == GTEXT_CSS_VALUE_FUNCTION && ieq(&v[a].value, "type")) {
    inner = v[a].children;
    n = v[a].child_count;
    wrapped = true;
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &s->alloc;
  char * text = NULL;
  size_t length = 0;
  GTEXT_CSS_Status st = n ? gtext_css_serialize_values_buffer(inner, n, &so, &text, &length) : GTEXT_CSS_E_GRAMMAR;
  if (st == GTEXT_CSS_E_OOM) {
    return st;
  }
  if (st != GTEXT_CSS_OK) {
    return GTEXT_CSS_OK;
  }
  css_compiled_syntax * c = NULL;
  st = css_registry_compile_syntax(s->registry, text, length, &c);
  gtext_allocator_free(&s->alloc, text);
  if (st == GTEXT_CSS_E_OOM) {
    return st;
  }
  if (st != GTEXT_CSS_OK) {
    return GTEXT_CSS_OK;   /* not a syntax string, or a component that is no type: the rule is invalid, not the library */
  }
  if (!wrapped && (c->syntax->universal || c->syntax->ncomps != 1)) {
    css_compiled_syntax_free(c);
    return GTEXT_CSS_OK;
  }
  *type = c;
  *verdict = PRE_OK;
  return GTEXT_CSS_OK;
}

/// Whether the values are a <declaration-value> at the top level: one or more tokens, none of them a `;`, a `!`, a bad string or url or a closing bracket that opens nothing.
static bool declaration_value(const GTEXT_CSS_Component_Value * v, size_t a, size_t b) {
  trim(v, &a, &b);
  if (a == b) {
    return false;
  }
  for (size_t i = a; i < b; i++) {
    if (v[i].kind != GTEXT_CSS_VALUE_TOKEN) {
      continue;
    }
    switch (v[i].type) {
    case GTEXT_CSS_TOKEN_SEMICOLON:
    case GTEXT_CSS_TOKEN_BAD_STRING:
    case GTEXT_CSS_TOKEN_BAD_URL:
    case GTEXT_CSS_TOKEN_CLOSE_PAREN:
    case GTEXT_CSS_TOKEN_CLOSE_SQUARE:
    case GTEXT_CSS_TOKEN_CLOSE_CURLY:
      return false;
    case GTEXT_CSS_TOKEN_DELIM:
      if (v[i].delim == '!') {
        return false;
      }
      break;
    default:
      break;
    }
  }
  return true;
}

/// One parameter: `--name [<type>] [: <default>]`. On a PRE_OK verdict `*p` is filled (owned by the caller).
static GTEXT_CSS_Status read_param(GTEXT_CSS_Function_Set * s, const GTEXT_CSS_Component_Value * v, size_t a, size_t b, css_fn_param * p, pre_verdict * verdict) {
  memset(p, 0, sizeof *p);
  *verdict = PRE_BAD_PARAMETER;
  trim(v, &a, &b);
  if (a == b) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Component_Value * nm = &v[a];
  if (nm->kind != GTEXT_CSS_VALUE_TOKEN || nm->type != GTEXT_CSS_TOKEN_IDENT || !css_creg_name_ok(nm->value.data, nm->value.length)) {
    return GTEXT_CSS_OK;
  }
  size_t colon = b;
  for (size_t k = a + 1; k < b; k++) {
    if (v[k].kind == GTEXT_CSS_VALUE_TOKEN && v[k].type == GTEXT_CSS_TOKEN_COLON) {
      colon = k;
      break;
    }
  }
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  pre_verdict tv = PRE_OK;
  size_t ta = a + 1, tb = colon;
  trim(v, &ta, &tb);
  if (ta < tb) {
    st = read_type(s, v, ta, tb, &p->type, &tv);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (tv != PRE_OK) {
      *verdict = PRE_BAD_TYPE;
      return GTEXT_CSS_OK;
    }
  }
  if (colon < b) {
    size_t da = colon + 1, db = b;
    trim(v, &da, &db);
    if (!declaration_value(v, da, db)) {
      css_compiled_syntax_free(p->type);
      p->type = NULL;
      *verdict = PRE_BAD_DEFAULT;
      return GTEXT_CSS_OK;
    }
    if (p->type && !p->type->syntax->universal) {
      /* "If a default value and a parameter type are both provided, then the default value must parse successfully according to that parameter type's syntax." A default
         that holds a substitution function is only a value when it is substituted: it is checked then. */
      bool sub = false;
      st = gtext_css_has_arbitrary_substitution(v + da, db - da, &s->alloc, &sub);
      if (st != GTEXT_CSS_OK) {
        css_compiled_syntax_free(p->type);
        p->type = NULL;
        return st;
      }
      if (!sub) {
        bool matched = false;
        css_syn_match m;
        memset(&m, 0, sizeof m);
        st = css_syntax_match(p->type->syntax, p->type->compiled, v + da, db - da, css_registry_match_options(s->registry), &matched, &m);
        gtext_css_value_set_free(m.set);
        if (st != GTEXT_CSS_OK) {
          css_compiled_syntax_free(p->type);
          p->type = NULL;
          return st;
        }
        if (!matched) {
          css_compiled_syntax_free(p->type);
          p->type = NULL;
          *verdict = PRE_BAD_DEFAULT;
          return GTEXT_CSS_OK;
        }
      }
    }
    GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
    so.allocator = &s->alloc;
    char * text = NULL;
    size_t length = 0;
    st = gtext_css_serialize_values_buffer(v + da, db - da, &so, &text, &length);
    if (st != GTEXT_CSS_OK) {
      css_compiled_syntax_free(p->type);
      p->type = NULL;
      return st;
    }
    p->default_text = text;
    p->default_len = length;
    p->has_default = true;
  }
  p->name = copy_n(&s->alloc, nm->value.data, nm->value.length);
  if (!p->name) {
    css_compiled_syntax_free(p->type);
    p->type = NULL;
    gtext_allocator_free(&s->alloc, p->default_text);
    p->default_text = NULL;
    return GTEXT_CSS_E_OOM;
  }
  p->name_len = nm->value.length;
  *verdict = PRE_OK;
  return GTEXT_CSS_OK;
}

static void param_free(const GTEXT_Allocator * a, css_fn_param * p) {
  gtext_allocator_free((GTEXT_Allocator *)a, p->name);
  css_compiled_syntax_free(p->type);
  gtext_allocator_free((GTEXT_Allocator *)a, p->default_text);
  memset(p, 0, sizeof *p);
}

/// The prelude of a rule: `--name(<parameters>) [returns <type>]`. On PRE_OK, `*def` is the definition (owned) without its body facts.
static GTEXT_CSS_Status read_prelude(GTEXT_CSS_Function_Set * s, const GTEXT_CSS_Rule * rule, css_fn_def ** def, pre_verdict * verdict) {
  *def = NULL;
  *verdict = PRE_BAD_PRELUDE;
  const GTEXT_CSS_Component_Value * p = rule->prelude;
  size_t a = 0, b = rule->prelude_count;
  trim(p, &a, &b);
  if (a == b) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Component_Value * fn = &p[a];
  if (fn->kind != GTEXT_CSS_VALUE_FUNCTION) {
    return GTEXT_CSS_OK;   /* `--foo (--x)` is an identifier and a block */
  }
  if (!css_creg_name_ok(fn->value.data, fn->value.length)) {
    *verdict = PRE_BAD_NAME;
    return GTEXT_CSS_OK;
  }
  css_fn_def * d = gtext_allocator_calloc(&s->alloc, 1, sizeof *d);
  if (!d) {
    return GTEXT_CSS_E_OOM;
  }
  d->alloc = s->alloc;
  d->name = copy_n(&s->alloc, fn->value.data, fn->value.length);
  if (!d->name) {
    gtext_allocator_free(&s->alloc, d);
    return GTEXT_CSS_E_OOM;
  }
  d->name_len = fn->value.length;
  d->rule = rule;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  /* the parameters, split at the commas of the function's own level */
  const GTEXT_CSS_Component_Value * args = fn->children;
  const size_t na = fn->child_count;
  size_t pa = 0, pb = na;
  trim(args, &pa, &pb);
  pre_verdict v = PRE_OK;
  if (pa < pb) {
    size_t count = 1;
    for (size_t i = pa; i < pb; i++) {
      count += args[i].kind == GTEXT_CSS_VALUE_TOKEN && args[i].type == GTEXT_CSS_TOKEN_COMMA;
    }
    if (count > s->opt.max_parameters) {
      v = PRE_BAD_PARAMETER;
    }
    else {
      d->params = gtext_allocator_calloc(&s->alloc, count, sizeof *d->params);
      if (!d->params) {
        st = GTEXT_CSS_E_OOM;
      }
      size_t from = pa;
      for (size_t i = pa; st == GTEXT_CSS_OK && v == PRE_OK && i <= pb; i++) {
        if (i < pb && !(args[i].kind == GTEXT_CSS_VALUE_TOKEN && args[i].type == GTEXT_CSS_TOKEN_COMMA)) {
          continue;
        }
        s->work++;
        css_fn_param prm;
        st = read_param(s, args, from, i, &prm, &v);
        if (st != GTEXT_CSS_OK) {
          break;
        }
        if (v == PRE_OK) {
          for (size_t k = 0; k < d->nparams; k++) {
            if (d->params[k].name_len == prm.name_len && memcmp(d->params[k].name, prm.name, prm.name_len) == 0) {
              v = PRE_DUPLICATE;
              break;
            }
          }
          if (v != PRE_OK) {
            param_free(&s->alloc, &prm);
          }
          else {
            d->params[d->nparams++] = prm;
          }
        }
        from = i + 1;
      }
    }
  }
  /* what follows: nothing, or `returns <type>` */
  if (st == GTEXT_CSS_OK && v == PRE_OK) {
    size_t ra = a + 1, rb = b;
    trim(p, &ra, &rb);
    if (ra < rb) {
      if (p[ra].kind != GTEXT_CSS_VALUE_TOKEN || p[ra].type != GTEXT_CSS_TOKEN_IDENT || !ieq(&p[ra].value, "returns")) {
        v = PRE_BAD_RETURNS;
      }
      else {
        pre_verdict tv = PRE_OK;
        st = read_type(s, p, ra + 1, rb, &d->returns, &tv);
        if (st == GTEXT_CSS_OK && tv != PRE_OK) {
          v = PRE_BAD_RETURNS;
        }
      }
    }
  }
  if (st != GTEXT_CSS_OK || v != PRE_OK) {
    css_fn_def_free(d);
    *verdict = v == PRE_OK ? PRE_BAD_PRELUDE : v;
    return st;
  }
  *def = d;
  *verdict = PRE_OK;
  return GTEXT_CSS_OK;
}

/* ---- the body ----------------------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Rule * rule;
  size_t item;
} body_frame;

static bool is_conditional(const GTEXT_CSS_Rule * r) {
  return r->kind == GTEXT_CSS_RULE_AT && (ieq(&r->name, "media") || ieq(&r->name, "supports") || ieq(&r->name, "container"));
}

/// Look through the body: whether it has a `result`, and which declarations are neither that nor a custom property (reported). Conditional group rules are entered with a stack of
/// their own, not the C stack.
static GTEXT_CSS_Status read_body(GTEXT_CSS_Function_Set * s, const GTEXT_CSS_Rule * rule, css_fn_def * d) {
  size_t cap = 16, top = 0;
  body_frame * stack = gtext_allocator_malloc(&s->alloc, cap * sizeof *stack);
  if (!stack) {
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  stack[top++] = (body_frame){rule, 0};
  while (top && st == GTEXT_CSS_OK) {
    body_frame * f = &stack[top - 1];
    if (f->item >= f->rule->content_count) {
      top--;
      continue;
    }
    const GTEXT_CSS_Block_Item * item = &f->rule->contents[f->item++];
    s->work++;
    if (item->kind == GTEXT_CSS_ITEM_RULE) {
      if (item->rule && is_conditional(item->rule)) {
        if (top >= s->opt.max_rule_depth) {
          st = GTEXT_CSS_E_LIMIT_DEPTH;
          break;
        }
        if (top == cap) {
          if (cap > SIZE_MAX / 2 / sizeof *stack) {
            st = GTEXT_CSS_E_OOM;
            break;
          }
          body_frame * bigger = gtext_allocator_realloc(&s->alloc, stack, cap * 2 * sizeof *stack);
          if (!bigger) {
            st = GTEXT_CSS_E_OOM;
            break;
          }
          stack = bigger;
          cap *= 2;
        }
        stack[top++] = (body_frame){item->rule, 0};
      }
      continue;
    }
    for (size_t k = 0; k < item->declaration_count && st == GTEXT_CSS_OK; k++) {
      const GTEXT_CSS_Declaration * decl = &item->declarations[k];
      s->work++;
      if (ieq(&decl->name, "result") && !decl->important) {
        d->has_result = true;
      }
      else if (decl->name.length > 2 && decl->name.data[0] == '-' && decl->name.data[1] == '-') {
        continue;   /* a local variable */
      }
      else {
        st = diagnose(s, GTEXT_CSS_FN_UNKNOWN_DESCRIPTOR, decl->name.data, decl->name.length, rule, &decl->span,
            "Unknown descriptors are invalid and ignored, but do not make the @function rule itself invalid (a result descriptor takes no !important).");
      }
    }
  }
  gtext_allocator_free(&s->alloc, stack);
  return st;
}

/* ---- one rule ----------------------------------------------------------------------------------------- */

/// Which of two layers the cascade sorts later: -1, 0 or 1 (as css_at_property.c: the order of two layers that exist does not change when another is declared).
static int layer_compare(const GTEXT_CSS_Function_Set * s, GTEXT_CSS_Layer_Id a, GTEXT_CSS_Layer_Id b) {
  if (a == b) {
    return 0;
  }
  if (a == GTEXT_CSS_LAYER_NONE) {
    return 1;
  }
  if (b == GTEXT_CSS_LAYER_NONE) {
    return -1;
  }
  enum { MAX = 257 };
  GTEXT_CSS_Layer_Id ca[MAX], cb[MAX];
  size_t na = 0, nb = 0;
  for (int side = 0; side < 2; side++) {
    GTEXT_CSS_Layer_Id * chain = side ? cb : ca;
    size_t n = 0;
    for (GTEXT_CSS_Layer_Id id = side ? b : a; id != GTEXT_CSS_LAYER_NONE && n < MAX;) {
      GTEXT_CSS_Layer_Info info;
      if (!gtext_css_layer_info(s->layers, id, &info)) {
        break;
      }
      chain[n++] = id;
      id = info.parent;
    }
    if (side) {
      nb = n;
    }
    else {
      na = n;
    }
  }
  size_t i = 0;
  while (i < na && i < nb && ca[na - 1 - i] == cb[nb - 1 - i]) {
    i++;
  }
  if (i == na) {
    return 1;
  }
  if (i == nb) {
    return -1;
  }
  return ca[na - 1 - i] < cb[nb - 1 - i] ? -1 : 1;
}

static const char * const kMessages[] = {
  [GTEXT_CSS_FN_BAD_PRELUDE] = "@function <function-token> <function-parameter>#? ) [ returns <css-type> ]? { ... }: the prelude is not that, so the rule is invalid.",
  [GTEXT_CSS_FN_BAD_NAME] = "\"The <function-token> production must start with two dashes (U+002D HYPHEN-MINUS), similar to <dashed-ident>, or else the definition is invalid.\"",
  [GTEXT_CSS_FN_BAD_PARAMETER] = "<function-parameter> = <custom-property-name> <css-type>? [ : <default-value> ]?: the parameter is not that, so the rule is invalid.",
  [GTEXT_CSS_FN_DUPLICATE_PARAMETER] = "\"If the function parameters contain the same <custom-property-name> more than once, then the @function rule is invalid.\"",
  [GTEXT_CSS_FN_BAD_TYPE] = "<css-type> = <syntax-component> | <type()>: a syntax that is not one component has to be wrapped in type(), so the rule is invalid.",
  [GTEXT_CSS_FN_BAD_DEFAULT] = "\"If a default value and a parameter type are both provided, then the default value must parse successfully according to that parameter type's syntax. Otherwise, the @function rule is invalid.\"",
  [GTEXT_CSS_FN_BAD_RETURNS] = "Only [ returns <css-type> ]? may follow the parameters: the rule is invalid.",
};

static GTEXT_CSS_Status handle_function(GTEXT_CSS_Function_Set * s, const GTEXT_CSS_Rule * rule, GTEXT_CSS_Layer_Id layer, bool * defined) {
  s->work++;
  if (defined) {
    *defined = false;
  }
  css_fn_def * d = NULL;
  pre_verdict verdict = PRE_BAD_PRELUDE;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (rule->has_block) {
    st = read_prelude(s, rule, &d, &verdict);
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (verdict != PRE_OK || !d) {
    const GTEXT_CSS_Function_Reason reason = (GTEXT_CSS_Function_Reason)(verdict == PRE_OK ? PRE_BAD_PRELUDE : verdict);
    return diagnose(s, reason, "", 0, rule, &rule->span, kMessages[reason]);
  }
  st = read_body(s, rule, d);
  if (st != GTEXT_CSS_OK) {
    css_fn_def_free(d);
    return st;
  }
  d->layer = layer;
  d->sheet = s->sheets ? s->sheets - 1 : 0;
  bool found = false;
  const size_t at = search(s, d->name, d->name_len, &found, &s->work);
  if (!found && s->ndefs >= s->opt.max_functions) {
    css_fn_def_free(d);
    return GTEXT_CSS_E_LIMIT_REGISTRY;
  }
  if (found) {
    css_fn_def * held = s->defs[at];
    if (layer_compare(s, layer, held->layer) < 0) {
      /* "the rule in the stronger cascade layer wins": a later rule in a weaker layer does not replace it */
      st = diagnose(s, GTEXT_CSS_FN_SUPERSEDED, d->name, d->name_len, rule, &rule->span,
          "\"If more than one @function exists for a given name, then the rule in the stronger cascade layer wins, and rules defined later win within the same layer.\"");
      css_fn_def_free(d);
      return st;
    }
    const GTEXT_CSS_Rule * lost = held->rule;
    s->defs[at] = d;
    st = diagnose(s, GTEXT_CSS_FN_SUPERSEDED, held->name, held->name_len, lost, &lost->span,
        "\"If more than one @function exists for a given name, then the rule in the stronger cascade layer wins, and rules defined later win within the same layer.\"");
    css_fn_def_free(held);
    if (defined) {
      *defined = true;
    }
    return st;
  }
  if (s->ndefs == s->defcap) {
    const size_t cap = s->defcap ? s->defcap * 2 : 8;
    css_fn_def ** bigger = gtext_allocator_realloc(&s->alloc, s->defs, cap * sizeof *bigger);
    if (!bigger) {
      css_fn_def_free(d);
      return GTEXT_CSS_E_OOM;
    }
    s->defs = bigger;
    s->defcap = cap;
  }
  memmove(&s->defs[at + 1], &s->defs[at], (s->ndefs - at) * sizeof *s->defs);
  s->defs[at] = d;
  s->ndefs++;
  if (defined) {
    *defined = true;
  }
  return GTEXT_CSS_OK;
}

/* ---- the walk ----------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSS_Function_Set * set;
  const GTEXT_CSS_Rule * failing;
  const char * detail;
  GTEXT_CSS_Status active_failure;
} walk_state;

static bool walk_active(void * user, const GTEXT_CSS_Rule * rule) {
  walk_state * w = user;
  if (w->set->opt.active_status) {
    bool applies = false;
    const GTEXT_CSS_Status st = w->set->opt.active_status(w->set->opt.user, rule, &applies);
    if (st != GTEXT_CSS_OK) {
      if (w->active_failure == GTEXT_CSS_OK) {
        w->active_failure = st;
        w->failing = rule;
      }
      return false;
    }
    return applies;
  }
  return w->set->opt.active(w->set->opt.user, rule);
}

static GTEXT_CSS_Status walk_rule(void * user, const GTEXT_CSS_Rule * rule, const GTEXT_CSS_Layer_Rule_Context * ctx) {
  walk_state * w = user;
  if (!ieq(&rule->name, "function")) {
    return GTEXT_CSS_OK;
  }
  w->set->work++;
  if (ctx->in_style) {
    return GTEXT_CSS_OK;   /* not valid inside a style rule */
  }
  if (ctx->unresolved) {
    w->failing = rule;
    w->detail = "an @function is inside a conditional rule and no active callback was given";
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  const GTEXT_CSS_Status st = handle_function(w->set, rule, ctx->layer, NULL);
  if (st != GTEXT_CSS_OK) {
    w->failing = rule;
  }
  return st;
}

static GTEXT_CSS_Status walk_tree(GTEXT_CSS_Function_Set * s, const GTEXT_CSS_Tree * tree, GTEXT_CSS_Function_Result * result) {
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  if (!s->layers) {
    GTEXT_CSS_Layer_Options lo = gtext_css_layer_options_default();
    lo.allocator = &s->alloc;
    status = gtext_css_layer_registry_new(&lo, &s->layers);
  }
  walk_state w;
  memset(&w, 0, sizeof w);
  w.set = s;
  GTEXT_CSS_Layer_Result lr;
  memset(&lr, 0, sizeof lr);
  if (status == GTEXT_CSS_OK) {
    GTEXT_CSS_Layer_Collect_Options o;
    memset(&o, 0, sizeof o);
    o.user = &w;
    o.active = s->opt.active || s->opt.active_status ? walk_active : NULL;
    o.max_rule_depth = s->opt.max_rule_depth;
    o.on_rule = walk_rule;
    status = gtext_css_cascade_collect_layers(s->layers, GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_LAYER_NONE, tree, &o, &lr);
    if (status == GTEXT_CSS_OK && w.active_failure != GTEXT_CSS_OK) {
      status = w.active_failure;
    }
  }
  if (result) {
    result->status = status;
    result->message = gtext_css_status_string(status);
    result->detail = w.detail ? w.detail : lr.detail;
    if (w.failing) {
      result->span = w.failing->span;
    }
    else if (status != GTEXT_CSS_OK) {
      result->span = lr.span;
    }
    result->work = lr.work >= s->layer_work ? lr.work - s->layer_work : 0;
  }
  if (s->layers && status != GTEXT_CSS_E_OOM) {
    s->layer_work = lr.work ? lr.work : s->layer_work;
  }
  return status;
}

static void fill_result(GTEXT_CSS_Function_Result * result, GTEXT_CSS_Status status) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->status = status;
    result->message = gtext_css_status_string(status);
  }
}

GTEXT_CSS_Status gtext_css_function_collect(GTEXT_CSS_Function_Set * s, const GTEXT_CSS_Tree * tree, GTEXT_CSS_Function_Result * result) {
  fill_result(result, GTEXT_CSS_OK);
  if (!s || !tree) {
    fill_result(result, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  s->sheets++;
  return walk_tree(s, tree, result);
}

GTEXT_CSS_Status gtext_css_function_collect_rules(GTEXT_CSS_Function_Set * s, const GTEXT_CSS_Rule * const * rules, size_t count, GTEXT_CSS_Function_Result * result) {
  fill_result(result, GTEXT_CSS_OK);
  if (!s || (count && !rules)) {
    fill_result(result, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  for (size_t i = 0; i < count; i++) {
    if (!rules[i]) {
      fill_result(result, GTEXT_CSS_E_INVALID);
      return GTEXT_CSS_E_INVALID;
    }
  }
  s->sheets++;
  size_t work = 0;
  for (size_t i = 0; i < count; i++) {
    GTEXT_CSS_Tree one;
    memset(&one, 0, sizeof one);
    one.kind = GTEXT_CSS_TREE_STYLESHEET;
    one.rules = rules[i];
    one.rule_count = 1;
    GTEXT_CSS_Function_Result r;
    const GTEXT_CSS_Status st = walk_tree(s, &one, &r);
    work += r.work;
    if (st != GTEXT_CSS_OK) {
      if (result) {
        *result = r;
        result->work = work;
      }
      return st;
    }
  }
  if (result) {
    result->work = work;
  }
  return GTEXT_CSS_OK;
}
