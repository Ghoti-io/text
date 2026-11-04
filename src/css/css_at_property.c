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
 * `@property` rules (css_at_property.h).
 */

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_at_property.h>
#include <ghoti.io/text/css/css_layer.h>

#include <string.h>

#include "css_creg.h"
#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_syntax_string.h"

#define DEFAULT_MAX_PROPERTIES ((size_t)4096)
#define LIMIT_MAX_PROPERTIES ((size_t)1 << 24)
#define DEFAULT_MAX_DIAGNOSTICS ((size_t)1024)
#define LIMIT_MAX_DIAGNOSTICS ((size_t)1 << 20)
#define DEFAULT_RULE_DEPTH ((size_t)4096)
#define LIMIT_RULE_DEPTH ((size_t)1 << 20)

typedef struct {
  GTEXT_CSS_At_Property_Reason reason;
  char * name;
  size_t name_len;
  GTEXT_CSS_Id rule;
  GTEXT_CSS_Span span;
  size_t sheet;
  const char * message;
} diag_rec;

struct GTEXT_CSS_At_Property_Set {
  GTEXT_Allocator alloc;
  const GTEXT_CSS_Registry * registry;
  GTEXT_CSS_At_Property_Options opt;
  css_creg_store store;
  diag_rec * diags;
  size_t ndiags, diagcap;
  size_t total;
  size_t sheets;
  size_t work;
  GTEXT_CSS_Layer_Registry * layers;   ///< The layers the rules read are in, in the order they were first declared (made on the first walk).
  size_t layer_work;                   ///< `layers`' own count of work, as the last walk left it.
};

GTEXT_CSS_At_Property_Options gtext_css_at_property_options_default(void) {
  GTEXT_CSS_At_Property_Options o;
  memset(&o, 0, sizeof o);
  o.max_properties = DEFAULT_MAX_PROPERTIES;
  o.max_diagnostics = DEFAULT_MAX_DIAGNOSTICS;
  o.max_rule_depth = DEFAULT_RULE_DEPTH;
  return o;
}

static size_t clamp(size_t v, size_t dflt, size_t max) {
  return v == 0 ? dflt : (v > max ? max : v);
}

GTEXT_CSS_Status gtext_css_at_property_set_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_At_Property_Options * options,
    GTEXT_CSS_At_Property_Set ** out) {
  if (!registry || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * ap = options && options->allocator ? options->allocator : css_registry_allocator(registry);
  GTEXT_Allocator a = *ap;
  GTEXT_CSS_At_Property_Set * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  s->alloc = a;
  s->registry = registry;
  if (options) {
    s->opt = *options;
  }
  s->opt.allocator = NULL;
  s->opt.max_properties = clamp(s->opt.max_properties, DEFAULT_MAX_PROPERTIES, LIMIT_MAX_PROPERTIES);
  s->opt.max_diagnostics = clamp(s->opt.max_diagnostics, DEFAULT_MAX_DIAGNOSTICS, LIMIT_MAX_DIAGNOSTICS);
  s->opt.max_rule_depth = clamp(s->opt.max_rule_depth, DEFAULT_RULE_DEPTH, LIMIT_RULE_DEPTH);
  css_creg_store_init(&s->store, &a);
  *out = s;
  return GTEXT_CSS_OK;
}

void gtext_css_at_property_set_free(GTEXT_CSS_At_Property_Set * s) {
  if (!s) {
    return;
  }
  GTEXT_Allocator a = s->alloc;
  for (size_t i = 0; i < s->ndiags; i++) {
    gtext_allocator_free(&a, s->diags[i].name);
  }
  gtext_allocator_free(&a, s->diags);
  css_creg_store_free(&s->store);
  gtext_css_layer_registry_free(s->layers);
  gtext_allocator_free(&a, s);
}

const char * gtext_css_at_property_reason_name(GTEXT_CSS_At_Property_Reason reason) {
  switch (reason) {
  case GTEXT_CSS_AT_PROP_BAD_NAME: return "bad-name";
  case GTEXT_CSS_AT_PROP_NO_SYNTAX: return "no-syntax";
  case GTEXT_CSS_AT_PROP_BAD_SYNTAX: return "bad-syntax";
  case GTEXT_CSS_AT_PROP_NO_INHERITS: return "no-inherits";
  case GTEXT_CSS_AT_PROP_NO_INITIAL: return "no-initial-value";
  case GTEXT_CSS_AT_PROP_BAD_INITIAL: return "bad-initial-value";
  case GTEXT_CSS_AT_PROP_NOT_INDEPENDENT: return "not-computationally-independent";
  case GTEXT_CSS_AT_PROP_UNKNOWN_DESCRIPTOR: return "unknown-descriptor";
  case GTEXT_CSS_AT_PROP_SUPERSEDED: return "superseded";
  case GTEXT_CSS_AT_PROP_CALLER_WINS: return "caller-registration-wins";
  default: return "unknown";
  }
}

static GTEXT_CSS_Status diagnose(GTEXT_CSS_At_Property_Set * s, GTEXT_CSS_At_Property_Reason reason, const char * name, size_t name_len, const GTEXT_CSS_Rule * rule,
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

size_t gtext_css_at_property_diagnostic_total(const GTEXT_CSS_At_Property_Set * s) {
  return s ? s->total : 0;
}

bool gtext_css_at_property_diagnostic(const GTEXT_CSS_At_Property_Set * s, size_t index, GTEXT_CSS_At_Property_Diagnostic * out) {
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

size_t gtext_css_at_property_count(const GTEXT_CSS_At_Property_Set * s) {
  return s ? s->store.count : 0;
}

static void fill_info(const css_creg * c, GTEXT_CSS_Custom_Info * info) {
  info->name = c->name;
  info->name_length = c->name_len;
  info->syntax = c->syntax_text;
  info->syntax_length = c->syntax_len;
  info->universal = c->syntax->universal;
  info->inherits = c->inherits;
  info->has_initial = c->has_initial;
  info->initial_value = c->has_initial ? c->initial : NULL;
  info->initial_length = c->has_initial ? c->initial_len : 0;
  info->component_count = c->syntax->universal ? 0 : c->syntax->ncomps;
}

bool gtext_css_at_property_at(const GTEXT_CSS_At_Property_Set * s, size_t index, GTEXT_CSS_Custom_Info * info) {
  if (!s || !info || index >= s->store.count) {
    return false;
  }
  fill_info(s->store.items[index], info);
  return true;
}

bool gtext_css_at_property_find(const GTEXT_CSS_At_Property_Set * s, const char * name, size_t length, GTEXT_CSS_Custom_Info * info) {
  const css_creg * c = s && name ? css_creg_store_find(&s->store, name, length, NULL) : NULL;
  if (!c || !info) {
    return false;
  }
  fill_info(c, info);
  return true;
}

/// The registrations as the compute context reads them.
const css_creg_store * css_at_property_store(const GTEXT_CSS_At_Property_Set * s) {
  return s ? &s->store : NULL;
}

/* ---- one rule ----------------------------------------------------------------------------------------- */

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

/// The one non-white-space value of a list, or NULL when there are none or more.
static const GTEXT_CSS_Component_Value * only_value(const GTEXT_CSS_Component_Value * v, size_t n) {
  const GTEXT_CSS_Component_Value * found = NULL;
  for (size_t i = 0; i < n; i++) {
    if (is_ws(&v[i])) {
      continue;
    }
    if (found) {
      return NULL;
    }
    found = &v[i];
  }
  return found;
}

typedef struct {
  char * syntax;       size_t syntax_len;  bool has_syntax;  bool saw_bad_syntax;
  bool inherits;       bool has_inherits;
  char * initial;      size_t initial_len; bool has_initial;
} descriptors;

static void descriptors_free(const GTEXT_Allocator * a, descriptors * d) {
  gtext_allocator_free((GTEXT_Allocator *)a, d->syntax);
  gtext_allocator_free((GTEXT_Allocator *)a, d->initial);
  memset(d, 0, sizeof *d);
}

static bool ieq(const GTEXT_CSS_String * s, const char * lower) {
  return css_ascii_ieq(s, lower);
}

/// The names of the prelude: `--a, --b`. Fills `names` (pointers into the tree) up to `cap`; `*count` is how many there are. False when the prelude is not
/// a list of custom property names.
static bool read_names(const GTEXT_CSS_Rule * rule, GTEXT_CSS_String * names, size_t cap, size_t * count) {
  size_t n = 0;
  size_t i = 0;
  const GTEXT_CSS_Component_Value * p = rule->prelude;
  const size_t pn = rule->prelude_count;
  bool expect_name = true;
  for (; i < pn; i++) {
    if (is_ws(&p[i])) {
      continue;
    }
    if (expect_name) {
      if (p[i].kind != GTEXT_CSS_VALUE_TOKEN || p[i].type != GTEXT_CSS_TOKEN_IDENT || !css_creg_name_ok(p[i].value.data, p[i].value.length)) {
        return false;
      }
      if (n < cap) {
        names[n] = p[i].value;
      }
      n++;
      expect_name = false;
    }
    else {
      return false; /* "@property <custom-property-name> { ... }": one name; a list, a comma and anything else are not a prelude of the rule */
    }
  }
  if (expect_name) {
    return false; /* empty */
  }
  *count = n;
  return true;
}

/// Read the descriptors of the rule's block. GTEXT_CSS_OK or GTEXT_CSS_E_OOM.
static GTEXT_CSS_Status read_descriptors(GTEXT_CSS_At_Property_Set * s, const GTEXT_CSS_Rule * rule, descriptors * d) {
  for (size_t it = 0; it < rule->content_count; it++) {
    const GTEXT_CSS_Block_Item * item = &rule->contents[it];
    if (item->kind != GTEXT_CSS_ITEM_DECLARATIONS) {
      continue; /* a rule inside an @property is not a descriptor */
    }
    for (size_t k = 0; k < item->declaration_count; k++) {
      const GTEXT_CSS_Declaration * decl = &item->declarations[k];
      s->work++;
      if (decl->important && (ieq(&decl->name, "syntax") || ieq(&decl->name, "inherits") || ieq(&decl->name, "initial-value"))) {
        /* a descriptor takes no !important: the declaration is invalid and ignored, the earlier one of its name stands (both browsers) */
        const GTEXT_CSS_Status st = diagnose(s, GTEXT_CSS_AT_PROP_UNKNOWN_DESCRIPTOR, decl->name.data, decl->name.length, rule, &decl->span,
            "A descriptor takes no !important: the declaration is invalid and ignored, and does not invalidate the @property rule.");
        if (st != GTEXT_CSS_OK) {
          return st;
        }
        continue;
      }
      if (ieq(&decl->name, "syntax")) {
        const GTEXT_CSS_Component_Value * v = only_value(decl->value, decl->value_count);
        css_syntax * syn = NULL;
        GTEXT_CSS_Status st = GTEXT_CSS_E_GRAMMAR;
        if (v && v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_STRING) {
          st = css_syntax_parse(&s->alloc, v->value.data ? v->value.data : "", v->value.length, &syn, NULL);
        }
        if (st == GTEXT_CSS_E_OOM) {
          return st;
        }
        if (st == GTEXT_CSS_OK) {
          css_syntax_free(syn);
          char * copy = gtext_allocator_malloc(&s->alloc, v->value.length + 1);
          if (!copy) {
            return GTEXT_CSS_E_OOM;
          }
          memcpy(copy, v->value.data, v->value.length);
          copy[v->value.length] = '\0';
          gtext_allocator_free(&s->alloc, d->syntax);
          d->syntax = copy;
          d->syntax_len = v->value.length;
          d->has_syntax = true;
        }
        else {
          /* "If the provided string is not a valid syntax string ... the descriptor is invalid and must be ignored." */
          d->saw_bad_syntax = true;
        }
      }
      else if (ieq(&decl->name, "inherits")) {
        const GTEXT_CSS_Component_Value * v = only_value(decl->value, decl->value_count);
        if (v && v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT && (ieq(&v->value, "true") || ieq(&v->value, "false"))) {
          d->inherits = ieq(&v->value, "true");
          d->has_inherits = true;
        }
      }
      else if (ieq(&decl->name, "initial-value")) {
        GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
        so.allocator = &s->alloc;
        char * text = NULL;
        size_t length = 0;
        const GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(decl->value, decl->value_count, &so, &text, &length);
        if (st != GTEXT_CSS_OK) {
          return st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_OK;
        }
        gtext_allocator_free(&s->alloc, d->initial);
        d->initial = text;
        d->initial_len = length;
        d->has_initial = true;
      }
      else {
        const GTEXT_CSS_Status st = diagnose(s, GTEXT_CSS_AT_PROP_UNKNOWN_DESCRIPTOR, decl->name.data, decl->name.length, rule, &decl->span,
            "Unknown descriptors are invalid and ignored, but do not invalidate the @property rule.");
        if (st != GTEXT_CSS_OK) {
          return st;
        }
      }
    }
  }
  return GTEXT_CSS_OK;
}

/// Whether a rule with these descriptors can be valid before the initial value is looked at: the one place the draft's "valid @property rule" is decided for
/// `syntax` and `inherits`. Returns the reason it cannot, or -1.
static int rule_valid(const descriptors * d) {
  if (!d->has_syntax) {
    return d->saw_bad_syntax ? GTEXT_CSS_AT_PROP_BAD_SYNTAX : GTEXT_CSS_AT_PROP_NO_SYNTAX;
  }
  if (!d->has_inherits) {
    return GTEXT_CSS_AT_PROP_NO_INHERITS;
  }
  return -1;
}

/// Which of two layers of a set's registry the cascade sorts later: -1, 0 or 1. The unlayered rules come last; a layer's sub-layers before its own rules; siblings in the order they were
/// first declared (which is the order of their ids). The order of two layers that exist does not change when another is declared, so it can be asked as the rules are read.
static int layer_compare(const GTEXT_CSS_At_Property_Set * s, GTEXT_CSS_Layer_Id a, GTEXT_CSS_Layer_Id b) {
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
  /* the chains run from the layer up to the top: compare from the top down */
  size_t i = 0;
  while (i < na && i < nb && ca[na - 1 - i] == cb[nb - 1 - i]) {
    i++;
  }
  if (i == na) {
    return 1;   /* `a` holds `b`: a layer's own rules come after its sub-layers */
  }
  if (i == nb) {
    return -1;
  }
  return ca[na - 1 - i] < cb[nb - 1 - i] ? -1 : 1;
}

static GTEXT_CSS_Status handle_property(GTEXT_CSS_At_Property_Set * s, const GTEXT_CSS_Rule * rule, GTEXT_CSS_Layer_Id layer) {
  s->work++;
  enum { NAME_CAP = 64 };
  GTEXT_CSS_String names[NAME_CAP];
  size_t nnames = 0;
  GTEXT_CSS_Status st;
  if (!rule->has_block || !read_names(rule, names, NAME_CAP, &nnames) || nnames > NAME_CAP) {
    return diagnose(s, GTEXT_CSS_AT_PROP_BAD_NAME, "", 0, rule, &rule->span,
        "@property <custom-property-name> { <declaration-list> }: the prelude is one custom property name.");
  }
  descriptors d;
  memset(&d, 0, sizeof d);
  st = read_descriptors(s, rule, &d);
  if (st != GTEXT_CSS_OK) {
    descriptors_free(&s->alloc, &d);
    return st;
  }
  const int bad = rule_valid(&d);
  if (bad >= 0) {
    static const char * const messages[] = {
      [GTEXT_CSS_AT_PROP_NO_SYNTAX] = "The @property rule needs a syntax descriptor: it is ignored.",
      [GTEXT_CSS_AT_PROP_BAD_SYNTAX] = "\"If the provided string is not a valid syntax string ... the descriptor is invalid and must be ignored\", so the rule has no syntax: it is ignored.",
      [GTEXT_CSS_AT_PROP_NO_INHERITS] = "The @property rule needs an inherits descriptor (true or false): it is ignored.",
    };
    st = diagnose(s, (GTEXT_CSS_At_Property_Reason)bad, names[0].data, names[0].length, rule, &rule->span, messages[bad]);
    descriptors_free(&s->alloc, &d);
    return st;
  }
  for (size_t i = 0; i < nnames && st == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_String * nm = &names[i];
    if (css_registry_find_creg(s->registry, nm->data, nm->length, &s->work)) {
      st = diagnose(s, GTEXT_CSS_AT_PROP_CALLER_WINS, nm->data, nm->length, rule, &rule->span,
          "\"A custom property registration from CSS.registerProperty() further wins over any @property rules for the same <custom-property-name>.\"");
      continue;
    }
    css_creg * c = NULL;
    GTEXT_CSS_Registry_Result rr;
    css_creg_why why = CSS_CREG_OK;
    st = css_registry_make_creg(s->registry, nm->data, nm->length, d.syntax, d.syntax_len, d.inherits, d.has_initial ? d.initial : NULL, d.initial_len, &c, &rr,
        &why);
    if (st == GTEXT_CSS_E_INVALID && why != CSS_CREG_OK) {
      GTEXT_CSS_At_Property_Reason reason = GTEXT_CSS_AT_PROP_BAD_INITIAL;
      const char * message = "The initial-value must successfully parse according to the rule's syntax descriptor, or else the descriptor is invalid and ignored.";
      switch (why) {
      case CSS_CREG_NO_INITIAL:
        reason = GTEXT_CSS_AT_PROP_NO_INITIAL;
        message = "A syntax other than * needs an initial-value: the rule is ignored.";
        break;
      case CSS_CREG_SUBSTITUTION:
      case CSS_CREG_NOT_INDEPENDENT:
        reason = GTEXT_CSS_AT_PROP_NOT_INDEPENDENT;
        message = "\"If parsed initial value is not computationally independent\" the registration is refused: the rule is ignored.";
        break;
      default:
        break;
      }
      /* an initial value that does not hold for the universal syntax is a descriptor to ignore, and the rule stands without it */
      if (why == CSS_CREG_BAD_INITIAL || why == CSS_CREG_SUBSTITUTION) {
        css_syntax * syn = NULL;
        const GTEXT_CSS_Status parsed = css_syntax_parse(&s->alloc, d.syntax, d.syntax_len, &syn, NULL);
        if (parsed == GTEXT_CSS_E_OOM) {
          st = parsed; /* a failed allocation is the answer, not a reason to ignore the rule */
          break;
        }
        if (parsed == GTEXT_CSS_OK) {
          const bool universal = syn->universal;
          css_syntax_free(syn);
          if (universal) {
            st = diagnose(s, reason, nm->data, nm->length, rule, &rule->span, message);
            if (st != GTEXT_CSS_OK) {
              break;
            }
            st = css_registry_make_creg(s->registry, nm->data, nm->length, d.syntax, d.syntax_len, d.inherits, NULL, 0, &c, &rr, &why);
            if (st == GTEXT_CSS_OK) {
              goto put;
            }
            if (st == GTEXT_CSS_E_OOM) {
              break;
            }
          }
        }
      }
      st = diagnose(s, reason, nm->data, nm->length, rule, &rule->span, message);
      continue;
    }
    if (st != GTEXT_CSS_OK) {
      break;
    }
  put:
    if (s->store.count >= s->opt.max_properties && !css_creg_store_find(&s->store, nm->data, nm->length, NULL)) {
      css_creg_free(c);
      st = GTEXT_CSS_E_LIMIT_REGISTRY;
      break;
    }
    c->layer = layer;
    const css_creg * held = css_creg_store_find(&s->store, nm->data, nm->length, NULL);
    if (held && layer_compare(s, layer, held->layer) < 0) {
      /* "the last one in stylesheet order wins" is the cascade's order: a rule in an earlier layer than the registration there is does not replace it */
      css_creg_free(c);
      st = diagnose(s, GTEXT_CSS_AT_PROP_SUPERSEDED, nm->data, nm->length, rule, &rule->span,
          "A later rule in an earlier cascade layer does not replace the registration of a rule in a later layer (Cascade 5, 6.4.3: \"Unlayered rules are sorted later than any layered rules\").");
      continue;
    }
    bool replaced = false;
    st = css_creg_store_put(&s->store, c, &replaced);
    if (st != GTEXT_CSS_OK) {
      css_creg_free(c);
      break;
    }
    if (replaced) {
      st = diagnose(s, GTEXT_CSS_AT_PROP_SUPERSEDED, nm->data, nm->length, rule, &rule->span,
          "\"If multiple valid @property rules represent a registration for the same <custom-property-name>, the last one in stylesheet order wins.\"");
    }
  }
  descriptors_free(&s->alloc, &d);
  return st;
}

GTEXT_CSS_Status css_at_property_rule_validity(const GTEXT_CSS_Registry * registry, const GTEXT_Allocator * allocator, const GTEXT_CSS_Rule * rule, bool * valid,
    const char ** why) {
  GTEXT_CSS_At_Property_Options opt = gtext_css_at_property_options_default();
  opt.allocator = allocator;
  GTEXT_CSS_At_Property_Set * s = NULL;
  GTEXT_CSS_Status st = gtext_css_at_property_set_new(registry, &opt, &s);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = handle_property(s, rule, GTEXT_CSS_LAYER_NONE);
  if (st == GTEXT_CSS_OK) {
    /* registered, or refused only because the caller registered the name itself (the rule is valid; that registration wins) */
    bool wins = false;
    for (size_t i = 0; i < s->ndiags; i++) {
      wins = wins || s->diags[i].reason == GTEXT_CSS_AT_PROP_CALLER_WINS;
    }
    *valid = s->store.count > 0 || wins;
    *why = NULL;
    if (!*valid && s->ndiags > 0) {
      *why = s->diags[0].message;
    }
  }
  gtext_css_at_property_set_free(s);
  return st;
}

/* ---- the walk ------------------------------------------------------------------------------------------- */

/// What the reader of css_layer.h hands back to this one while it walks a tree.
typedef struct {
  GTEXT_CSS_At_Property_Set * set;
  const GTEXT_CSS_Rule * failing;
  const char * detail;
  GTEXT_CSS_Status active_failure;   ///< The first failure of `active_status`: the walk went on as if the rule did not apply, and the call reports it.
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

/// An at-rule the layers' reader reached and did not handle: an `@property` is registered with the layer it is in.
static GTEXT_CSS_Status walk_rule(void * user, const GTEXT_CSS_Rule * rule, const GTEXT_CSS_Layer_Rule_Context * ctx) {
  walk_state * w = user;
  if (!ieq(&rule->name, "property")) {
    return GTEXT_CSS_OK;
  }
  w->set->work++;
  if (ctx->in_style) {
    return GTEXT_CSS_OK;   /* not valid inside a style rule */
  }
  if (ctx->unresolved) {
    w->failing = rule;
    w->detail = "an @property is inside a conditional rule and no active callback was given";
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  const GTEXT_CSS_Status st = handle_property(w->set, rule, ctx->layer);
  if (st != GTEXT_CSS_OK) {
    w->failing = rule;
  }
  return st;
}

/// Read one tree of rules. The tree is read by the reader of css_layer.h, the one place that knows what an `@layer` rule is: `@layer` blocks and statements put the rules in their layers,
/// the conditional rules whose condition holds and `@scope` and `@starting-style` are entered, and an `@property` is registered where it stands (Cascade 5 orders it by layer, then by place).
static GTEXT_CSS_Status walk_tree(GTEXT_CSS_At_Property_Set * s, const GTEXT_CSS_Tree * tree, GTEXT_CSS_At_Property_Result * result) {
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

/// Fill a result from a status.
static void fill_result(GTEXT_CSS_At_Property_Result * result, GTEXT_CSS_Status status) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->status = status;
    result->message = gtext_css_status_string(status);
  }
}

GTEXT_CSS_Status gtext_css_at_property_collect(GTEXT_CSS_At_Property_Set * s, const GTEXT_CSS_Tree * tree, GTEXT_CSS_At_Property_Result * result) {
  fill_result(result, GTEXT_CSS_OK);
  if (!s || !tree) {
    fill_result(result, GTEXT_CSS_E_INVALID);
    return GTEXT_CSS_E_INVALID;
  }
  s->sheets++;
  return walk_tree(s, tree, result);
}

GTEXT_CSS_Status gtext_css_at_property_collect_rules(GTEXT_CSS_At_Property_Set * s, const GTEXT_CSS_Rule * const * rules, size_t count, GTEXT_CSS_At_Property_Result * result) {
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
    /* a tree of the one rule: the reader looks at nothing else of a tree, so the rules of a sheet that keep their own trees are read as the one sheet they are */
    GTEXT_CSS_Tree one;
    memset(&one, 0, sizeof one);
    one.kind = GTEXT_CSS_TREE_STYLESHEET;
    one.rules = rules[i];
    one.rule_count = 1;
    GTEXT_CSS_At_Property_Result r;
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
