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
 * The leaves of a `@supports` condition (CSS Conditional 3 and 5): a declaration, `selector()`, `font-tech()`, `font-format()`, `at-rule()`, `named-feature()` and `env()`.
 * A declaration is supported when the registry knows the property and the value is valid for it by the parser the cascade uses, with quirks off.
 */

#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_property.h>

#include <string.h>

#include "css_condition_internal.h"

/// The at-rules this library implements (the ledger's applies or typed column), lower case. `@charset` is not among them: Syntax 3, 3.2 makes it an encoding
/// declaration the decoder reads, not a rule that is parsed (it is not in the CSSOM), and both browsers answer false.
static const char * const implemented_at_rules[] = {"media", "supports", "container", "when", "else", "starting-style", "custom-media", "import", "namespace", "layer",
    "scope", "property", "font-face", "font-feature-values", "font-palette-values", "color-profile", "counter-style", "page", "keyframes", "-webkit-keyframes",
    "view-transition", "position-try"};

static bool at_rule_implemented(const char * name, size_t length) {
  for (size_t i = 0; i < sizeof implemented_at_rules / sizeof implemented_at_rules[0]; i++) {
    if (strlen(implemented_at_rules[i]) == length && memcmp(implemented_at_rules[i], name, length) == 0) {
      return true;
    }
  }
  return false;
}

/// Whether a selector list holds a forgiving list that is empty, skipped an empty entry (`:is(a,)`) or kept an entry it could not read: `selector()` is not supported then, whatever the list would do
/// in a style rule (the web-platform-tests: `selector(:is(:foo))` and `selector(:is())` are false).
static bool has_forgiven(const GTEXT_CSS_Selector_List * root, const GTEXT_Allocator * a, bool * oom) {
  size_t cap = 16, sp = 0;
  const GTEXT_CSS_Selector_List ** stack = gtext_allocator_malloc(a, cap * sizeof *stack);
  if (!stack) {
    *oom = true;
    return false;
  }
  bool found = false;
  stack[sp++] = root;
  while (sp && !found) {
    const GTEXT_CSS_Selector_List * l = stack[--sp];
    if (l->mode == GTEXT_CSS_SELECTOR_FORGIVING_LIST && (l->count == 0 || l->had_empty_entry)) {
      found = true;
      break;
    }
    for (size_t i = 0; i < l->count && !found; i++) {
      const GTEXT_CSS_Complex * cx = &l->complex[i];
      if (cx->raw.data) {
        found = true;
        break;
      }
      for (size_t j = 0; j < cx->compound_count && !found; j++) {
        for (size_t k = 0; k < cx->compounds[j].simple_count; k++) {
          const GTEXT_CSS_Simple * sm = &cx->compounds[j].simples[k];
          if (!sm->list) {
            continue;
          }
          if (sp == cap) {
            const GTEXT_CSS_Selector_List ** ns = gtext_allocator_realloc(a, stack, cap * 2 * sizeof *stack);
            if (!ns) {
              *oom = true;
              found = true;
              break;
            }
            stack = ns;
            cap *= 2;
          }
          stack[sp++] = sm->list;
        }
      }
    }
  }
  gtext_allocator_free(a, stack);
  return found;
}

static GTEXT_CSS_Tristate tri_of(bool b) {
  return b ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
}

/// Whether `name: value` is supported. *status is set on a failure. A custom property is supported with any value that is a `<declaration-value>` (the empty one
/// included): the registry's pending grammar says so, with no top-level `;` or `!`, no unmatched `)` `]` `}`, no bad string or bad url, and every `var()` valid.
static bool decl_supported(const GTEXT_CSS_Conditions * c, const char * name, size_t nl, const char * value, size_t vl, GTEXT_CSS_Status * status, const char ** detail) {
  if (!c->registry) {
    *status = GTEXT_CSS_E_UNSUPPORTED;
    *detail = "registry";
    return false;
  }
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &c->alloc;
  po.quirks = false; /* quirks never apply to @supports */
  GTEXT_CSS_Property_Value pv;
  GTEXT_CSS_Status st = gtext_css_parse_property_text(c->registry, name, nl, value, vl, &po, &pv);
  if (st == GTEXT_CSS_E_UNSUPPORTED) {
    gtext_css_property_value_free(&pv);
    return false; /* a property with no usable grammar is not supported */
  }
  if (st != GTEXT_CSS_OK) {
    *status = st;
    gtext_css_property_value_free(&pv);
    return false;
  }
  bool ok = pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE;
  const bool custom = nl >= 2 && name[0] == '-' && name[1] == '-';
  if (ok && !custom && c->policy.property_supported) {
    ok = c->policy.property_supported(c->policy.user, pv.property ? pv.property : pv.named);
  }
  gtext_css_property_value_free(&pv);
  return ok;
}

GTEXT_CSS_Tristate css_supports_eval_leaf(const GTEXT_CSS_Conditions * c, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  switch ((mq_node_kind)n->kind) {
  case MQN_DECL: {
    bool ok = decl_supported(c, n->text, n->text_length, n->value ? n->value : "", n->value_length, status, detail);
    return tri_of(ok);
  }
  case MQN_SELECTOR: {
    if (!n->sel) {
      return GTEXT_CSS_TRI_FALSE; /* not a selector: false, not unknown */
    }
    bool oom = false;
    bool forgiven = has_forgiven(n->sel, &c->alloc, &oom);
    if (oom) {
      *status = GTEXT_CSS_E_OOM;
      return GTEXT_CSS_TRI_UNKNOWN;
    }
    if (forgiven) {
      return GTEXT_CSS_TRI_FALSE;
    }
    if (n->sel->unmatchable) {
      return GTEXT_CSS_TRI_FALSE; /* a selector this library parses and can never match (`:nth-col()`) is not one it supports */
    }
    if (!c->iface) {
      return GTEXT_CSS_TRI_TRUE;
    }
    GTEXT_CSS_Status st = gtext_css_selector_check_interface(c->iface, n->sel, NULL);
    return tri_of(st == GTEXT_CSS_OK);
  }
  case MQN_FONT_TECH:
    return tri_of((c->policy.font_tech & n->bits) != 0);
  case MQN_FONT_FORMAT:
    return tri_of(n->bits != 0 && (c->policy.font_format & n->bits) != 0);
  case MQN_AT_RULE: {
    bool ok = at_rule_implemented(n->text, n->text_length);
    if (ok && c->policy.at_rule_supported) {
      GTEXT_CSS_String s = {n->text, n->text_length};
      ok = c->policy.at_rule_supported(c->policy.user, &s);
    }
    return tri_of(ok);
  }
  case MQN_NAMED: {
    GTEXT_CSS_String s = {n->text, n->text_length};
    return tri_of(c->policy.named_feature && c->policy.named_feature(c->policy.user, &s));
  }
  case MQN_ENV: {
    GTEXT_CSS_String s = {n->text, n->text_length};
    return tri_of(c->policy.env_variable && c->policy.env_variable(c->policy.user, &s));
  }
  default:
    return GTEXT_CSS_TRI_UNKNOWN;
  }
}

GTEXT_CSS_Status gtext_css_conditions_supports(const GTEXT_CSS_Conditions * c, const char * property, const char * value, bool * result) {
  if (!c || !property || !value || !result) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  const char * detail = NULL;
  bool ok = decl_supported(c, property, strlen(property), value, strlen(value), &status, &detail);
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  *result = ok;
  return GTEXT_CSS_OK;
}
