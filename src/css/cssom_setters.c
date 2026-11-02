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
 * The attributes of a rule that CSSOM names (`selectorText`, `keyText`, `conditionText`, `name`, the descriptors of `@counter-style`, `href`, `start` ...) and the setters among
 * them (css_cssom_rule.h). A rule whose syntax is immutable is **replaced as a whole**: the setter writes the candidate text of the rule's own part, reads it again in the place
 * the rule stands in (the place decides what is valid there), and swaps the rule's syntax for the result only if it is a rule of the same kind that holds what was set; else the
 * call is ignored (or, where the standard throws, refused) and nothing has changed. The cost is the size of that one rule.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_kind_tables.h"
#include "cssom_typed.h"

/* ---- reading a candidate -------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status css_own_prepare(css_rule * rule, const char * text, size_t length, css_own * own, bool * invalid) {
  css_core * core = rule->obj.core;
  memset(own, 0, sizeof *own);
  *invalid = false;
  css_insert_ctx ctx;
  GTEXT_CSSOM_Rule_Kind parent_kind;
  GTEXT_CSS_Rule synthetic;
  css_rule_own_ctx(rule, &ctx, &parent_kind, &synthetic);
  GTEXT_CSS_Tree * tree = NULL;
  bool syntax = false;
  GTEXT_CSS_Status st = css_parse_inserted(core, &ctx, text, length, &tree, &syntax);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (syntax) {
    *invalid = true;
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Rule * node = &tree->rules[0];
  const GTEXT_CSSOM_Rule_Kind kind = css_kind_of_node(node, parent_kind, parent_kind == GTEXT_CSSOM_RULE_UNKNOWN);
  if (kind != (GTEXT_CSSOM_Rule_Kind)rule->kind || (kind == GTEXT_CSSOM_RULE_LAYER && node->has_block != rule->has_block)) {
    gtext_css_tree_free(tree);
    *invalid = true;
    return GTEXT_CSS_OK;
  }
  css_source * source = NULL;
  const uint64_t saved = core->next_id;
  st = css_source_new(core, tree, NULL, 0, &source);
  if (st != GTEXT_CSS_OK) {
    gtext_css_tree_free(tree);
    return st;
  }
  css_source_set_ns(source, (css_ns_env *)(uintptr_t)ctx.ns);
  own->saved_id = saved;
  own->tree = tree;
  own->source = source;
  own->node = node;
  return GTEXT_CSS_OK;
}

void css_own_discard(css_own * own) {
  if (own->source && own->source->core->next_id == own->saved_id + 1) {
    own->source->core->next_id = own->saved_id;
  }
  css_source_release(own->source);
  gtext_css_tree_free(own->tree);
  memset(own, 0, sizeof *own);
}

void css_own_swap(css_rule * rule, css_own * own) {
  css_source * old = rule->source;
  rule->source = own->source;
  rule->node = own->node;
  rule->source_kind = GTEXT_CSSOM_SOURCE_MODIFIED;
  css_source_release(old);
  gtext_css_tree_free(own->tree);   /* the source holds its own reference */
  memset(own, 0, sizeof *own);
}

void css_own_commit(css_rule * rule, css_own * own) {
  css_own_swap(rule, own);
  css_rule_touch(rule);
}

GTEXT_CSS_Status css_own_typed(css_rule * rule, const css_own * own, bool shallow, GTEXT_CSS_Atrule ** out) {
  css_rule tmp = *rule;
  tmp.node = own->node;
  return css_rule_typed(rule->obj.core, &tmp, shallow, out);
}

void css_rule_replaced(css_rule * rule, GTEXT_CSS_Selector_List * before, bool before_failed, GTEXT_CSSOM_Outcome * outcome) {
  css_core * core = rule->obj.core;
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = GTEXT_CSSOM_EVENT_RULE_REPLACED;
  ev.selectors_before = before;
  ev.before_failed = before_failed;
  ev.rule = rule;
  ev.rule_id = rule->obj.id;
  css_sheet * sheet = css_rule_sheet(rule);
  if (sheet) {
    css_sheet_bump(sheet);
    ev.sheet = sheet;
    ev.sheet_id = sheet->obj.id;
  }
  else {
    ev.silent = true;
  }
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
  }
  css_obj_retain_external(&rule->obj);
  css_cssom_commit(core, &ev, outcome);
  css_obj_release_external(&rule->obj);
}

/* ---- the text of a rule's own part ----------------------------------------------------------------------------------------------------------- */

/// `from` is `0%` and `to` is `100%`; a selector list is written with ", " between its keys.
static GTEXT_CSS_Status key_text_of(const char * prelude, size_t length, tbuf * out) {
  size_t i = 0;
  bool first = true;
  while (i <= length) {
    size_t j = i;
    while (j < length && prelude[j] != ',') {
      j++;
    }
    size_t a = i, b = j;
    while (a < b && (prelude[a] == ' ' || prelude[a] == '\t' || prelude[a] == '\n')) {
      a++;
    }
    while (b > a && (prelude[b - 1] == ' ' || prelude[b - 1] == '\t' || prelude[b - 1] == '\n')) {
      b--;
    }
    const char * key = prelude + a;
    const size_t n = b - a;
    const bool from = n == 4 && (key[0] | 32) == 'f' && (key[1] | 32) == 'r' && (key[2] | 32) == 'o' && (key[3] | 32) == 'm';
    const bool to = n == 2 && (key[0] | 32) == 't' && (key[1] | 32) == 'o';
    if (!first && !tb_puts(out, ", ")) {
      return GTEXT_CSS_E_OOM;
    }
    first = false;
    if (!tb_put(out, from ? "0%" : to ? "100%" : key, from ? 2 : to ? 4 : n)) {
      return GTEXT_CSS_E_OOM;
    }
    i = j + 1;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_rule_header_text(css_rule * rule, tbuf * out) {
  css_core * core = rule->obj.core;
  if (rule->kind == GTEXT_CSSOM_RULE_STYLE) {
    return css_rule_selector_text(rule, out);
  }
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = css_rule_typed(core, rule, true, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  size_t pl = 0;
  const char * prelude = gtext_css_atrule_prelude_text(a, &pl);
  if (rule->kind == GTEXT_CSSOM_RULE_KEYFRAME) {
    st = key_text_of(prelude, pl, out);
  }
  else {
    const char * name = rule->node->name.data;
    if (!tb_puts(out, "@") || !tb_put(out, name, rule->node->name.length) || (pl && (!tb_puts(out, " ") || !tb_put(out, prelude, pl)))) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  gtext_css_atrule_free(a);
  return st;
}

/// The declarations of a typed rule's own block as text: `name: value;` each, from the typed model.
static GTEXT_CSS_Status own_body(css_rule * rule, tbuf * out) {
  if (rule->kind == GTEXT_CSSOM_RULE_FONT_FEATURE_VALUES) {
    return css_feature_values_body(rule, out);   /* the feature blocks are part of the body: a new family list must not drop them */
  }
  if (!css_typed_kind((GTEXT_CSSOM_Rule_Kind)rule->kind) && rule->kind != GTEXT_CSSOM_RULE_PROPERTY && rule->kind != GTEXT_CSSOM_RULE_COUNTER_STYLE) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = css_rule_typed(rule->obj.core, rule, false, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const size_t n = gtext_css_atrule_descriptor_count(a);
  for (size_t i = 0; i < n && st == GTEXT_CSS_OK; i++) {
    GTEXT_CSS_Atrule_Descriptor d;
    gtext_css_atrule_descriptor_at(a, i, &d);
    if (!tb_put(out, d.name, d.name_length) || !tb_puts(out, ": ") || !tb_put(out, d.text.data, d.text.length) || !tb_puts(out, d.important ? " !important; " : "; ")) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  gtext_css_atrule_free(a);
  return st;
}

/* ---- the whole-rule replacement -------------------------------------------------------------------------------------------------------------- */

/// Try `header { body }` as the rule's new own part. `body_override` replaces the rule's own declarations when not NULL. On success the rule is changed (and `check` has passed).
/// `*ignored` is true when the text does not read as this rule; nothing has changed then.

GTEXT_CSS_Status css_rule_replace_own(css_rule * rule, const char * header, size_t header_length, const char * body, size_t body_length, bool statement, own_check check, void * user,
    bool * ignored, GTEXT_CSSOM_Outcome * outcome) {
  const bool style = rule->kind == GTEXT_CSSOM_RULE_STYLE;
  css_core * core = rule->obj.core;
  *ignored = false;
  tbuf cand;
  tb_init(&cand, &core->alloc);
  if (!tb_put(&cand, header, header_length) || (statement ? !tb_puts(&cand, ";") : (!tb_puts(&cand, " {") || (body_length && !tb_put(&cand, body, body_length)) || !tb_puts(&cand, "}"))) || cand.oom) {
    tb_free(&cand);
    return GTEXT_CSS_E_OOM;
  }
  css_own own;
  bool invalid = false;
  GTEXT_CSS_Status st = css_own_prepare(rule, cand.data, cand.len, &own, &invalid);
  tb_free(&cand);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  bool ok = true;
  if (!invalid && check) {
    st = check(rule, &own, user, &ok);
    if (st != GTEXT_CSS_OK) {
      css_own_discard(&own);
      return st;
    }
  }
  if (invalid || !ok) {
    if (!invalid) {
      css_own_discard(&own);
    }
    *ignored = true;
    return GTEXT_CSS_OK;
  }
  if (style) {
    /* the block, not the syntax, is the truth from here on: built before the swap, while the call can still fail with nothing changed */
    st = css_rule_build_style(rule);
    if (st != GTEXT_CSS_OK) {
      css_own_discard(&own);
      return st;
    }
  }
  /* the selector list as it is now is what the event says it was: read while the rule still holds it. Running out of memory here is the call's failure, with nothing changed; a prelude
     that no longer reads as a selector list (it did when the rule was made) is the event's (`summary_complete`) */
  GTEXT_CSS_Selector_List * before = NULL;
  bool before_failed = false;
  if (style && rule->node && css_events_wanted(core) && css_rule_sheet(rule)) {
    st = css_rule_selector_list(rule, &before);
    if (st == GTEXT_CSS_E_OOM || st == GTEXT_CSS_E_LIMIT_WORK) {
      css_own_discard(&own);   /* a resource failure here is before the change, so it is a failure of the call and nothing changed */
      return st;
    }
    before_failed = st != GTEXT_CSS_OK;
  }
  css_own_commit(rule, &own);
  st = css_typed_refresh(rule);
  css_rule_replaced(rule, before, before_failed, outcome);
  return st;
}

/* ---- selectorText, keyText ---------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status ready(GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Outcome * outcome) {
  if (!rule) {
    return GTEXT_CSS_E_INVALID;
  }
  css_outcome_reset(outcome, rule->obj.core);
  return css_cssom_enter(rule->obj.core);
}

static GTEXT_CSS_Status take(tbuf * b, char ** out, size_t * length) {
  if (b->oom) {
    tb_free(b);
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  char * s = tb_take(b, &n);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  *out = s;
  if (length) {
    *length = n;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_rule_selector_text(GTEXT_CSSOM_Rule * rule, char ** out, size_t * length) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_STYLE && rule->kind != GTEXT_CSSOM_RULE_PAGE) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  tbuf b;
  tb_init(&b, &rule->obj.core->alloc);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (rule->kind == GTEXT_CSSOM_RULE_STYLE) {
    st = css_rule_selector_text(rule, &b);
  }
  else {
    GTEXT_CSS_Atrule * a = NULL;
    st = css_rule_typed(rule->obj.core, rule, true, &a);
    if (st == GTEXT_CSS_OK) {
      size_t n = 0;
      const char * p = gtext_css_atrule_prelude_text(a, &n);
      if (n && !tb_put(&b, p, n)) {
        st = GTEXT_CSS_E_OOM;
      }
      gtext_css_atrule_free(a);
    }
  }
  if (st != GTEXT_CSS_OK) {
    tb_free(&b);
    return st;
  }
  return take(&b, out, length);
}

GTEXT_CSS_Status gtext_cssom_rule_set_selector_text(GTEXT_CSSOM_Rule * rule, const char * text, size_t length, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if ((!text && length) || (rule && rule->kind != GTEXT_CSSOM_RULE_STYLE && rule->kind != GTEXT_CSSOM_RULE_PAGE)) {
    return rule ? GTEXT_CSS_E_UNSUPPORTED : GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = ready(rule, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (length > rule->obj.core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  /* "parse a group of selectors ... If the result is failure, do nothing" (CSSStyleRule.selectorText); an @page selector likewise */
  tbuf old, body;
  tb_init(&old, &rule->obj.core->alloc);
  tb_init(&body, &rule->obj.core->alloc);
  if (rule->kind != GTEXT_CSSOM_RULE_STYLE) {
    st = own_body(rule, &body);
  }
  tbuf header;
  tb_init(&header, &rule->obj.core->alloc);
  if (st == GTEXT_CSS_OK) {
    if (rule->kind == GTEXT_CSSOM_RULE_PAGE) {
      if (!tb_puts(&header, "@page") || (length && (!tb_puts(&header, " ") || !tb_put(&header, text, length)))) {
        st = GTEXT_CSS_E_OOM;
      }
    }
    else if (!tb_put(&header, text, length)) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  bool ignored = false;
  if (st == GTEXT_CSS_OK) {
    st = css_rule_replace_own(rule, header.data ? header.data : "", header.len, body.data, body.len, false, NULL, NULL, &ignored, out);
  }
  tb_free(&old);
  tb_free(&body);
  tb_free(&header);
  if (st == GTEXT_CSS_OK && ignored) {
    out->kind = GTEXT_CSSOM_IGNORED_INVALID_SELECTOR;
    out->detail = "CSSOM: the selector text does not parse as a selector list: nothing changes";
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_rule_key_text(GTEXT_CSSOM_Rule * rule, char ** out, size_t * length) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_KEYFRAME) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  tbuf b;
  tb_init(&b, &rule->obj.core->alloc);
  GTEXT_CSS_Status st = css_rule_header_text(rule, &b);
  if (st != GTEXT_CSS_OK) {
    tb_free(&b);
    return st;
  }
  return take(&b, out, length);
}

GTEXT_CSS_Status gtext_cssom_rule_set_key_text(GTEXT_CSSOM_Rule * rule, const char * text, size_t length, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if ((!text && length) || (rule && rule->kind != GTEXT_CSSOM_RULE_KEYFRAME)) {
    return rule ? GTEXT_CSS_E_UNSUPPORTED : GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = ready(rule, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (length > rule->obj.core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  tbuf body;
  tb_init(&body, &rule->obj.core->alloc);
  st = own_body(rule, &body);
  bool ignored = false;
  if (st == GTEXT_CSS_OK) {
    st = css_rule_replace_own(rule, text ? text : "", length, body.data, body.len, false, NULL, NULL, &ignored, out);
  }
  tb_free(&body);
  if (st == GTEXT_CSS_OK && ignored) {
    /* CSS Animations 1, CSSKeyframeRule.keyText: "On setting, ... if the key list fails to parse, throw a SyntaxError" (both browsers throw: css/cssom/CSSKeyframeRule-keyText.html) */
    out->kind = GTEXT_CSSOM_IGNORED_INVALID_SELECTOR;
    out->detail = "CSS Animations 1: a key list that does not parse is a SyntaxError";
    return GTEXT_CSS_E_SYNTAX;
  }
  return st;
}

/* ---- the textual attributes ----------------------------------------------------------------------------------------------------------------- */

typedef enum {
  SRC_NAME,          ///< The first name of the prelude, decoded; the prelude when it has none.
  SRC_PRELUDE,       ///< The prelude as the typed model writes it.
  SRC_DESC,          ///< The descriptor `desc`, unquoted when it is a string; null when absent.
  SRC_IMPORT_HREF,
  SRC_IMPORT_LAYER,
  SRC_IMPORT_SUPPORTS,
  SRC_NS_URI,
  SRC_NS_PREFIX,
  SRC_SCOPE_START,
  SRC_SCOPE_END,
  SRC_CONTAINER_NAME,
  SRC_CONTAINER_QUERY,
  SRC_MARGIN_NAME
} attr_src;

typedef struct {
  GTEXT_CSSOM_Rule_Kind kind;
  const char * idl;
  attr_src src;
  const char * desc;       ///< The descriptor, for SRC_DESC.
  bool writable;
} attr_row;

#define A(kind, idl, src, desc, w) {GTEXT_CSSOM_RULE_##kind, idl, src, desc, w}
static const attr_row attr_rows[] = {
  A(IMPORT, "href", SRC_IMPORT_HREF, NULL, false),
  A(IMPORT, "layerName", SRC_IMPORT_LAYER, NULL, false),
  A(IMPORT, "supportsText", SRC_IMPORT_SUPPORTS, NULL, false),
  A(NAMESPACE, "namespaceURI", SRC_NS_URI, NULL, false),
  A(NAMESPACE, "prefix", SRC_NS_PREFIX, NULL, false),
  A(LAYER, "name", SRC_PRELUDE, NULL, false),
  A(LAYER, "nameList", SRC_PRELUDE, NULL, false),
  A(SCOPE, "start", SRC_SCOPE_START, NULL, false),
  A(SCOPE, "end", SRC_SCOPE_END, NULL, false),
  A(CONTAINER, "containerName", SRC_CONTAINER_NAME, NULL, false),
  A(CONTAINER, "containerQuery", SRC_CONTAINER_QUERY, NULL, false),
  A(CONTAINER, "conditionText", SRC_PRELUDE, NULL, false),
  A(MEDIA, "conditionText", SRC_PRELUDE, NULL, false),
  A(SUPPORTS, "conditionText", SRC_PRELUDE, NULL, false),
  A(WHEN, "conditionText", SRC_PRELUDE, NULL, false),
  A(ELSE, "conditionText", SRC_PRELUDE, NULL, false),
  A(PROPERTY, "name", SRC_NAME, NULL, false),
  A(PROPERTY, "syntax", SRC_DESC, "syntax", false),
  A(PROPERTY, "inherits", SRC_DESC, "inherits", false),
  A(PROPERTY, "initialValue", SRC_DESC, "initial-value", false),
  A(COUNTER_STYLE, "name", SRC_NAME, NULL, true),
  A(COUNTER_STYLE, "system", SRC_DESC, "system", true),
  A(COUNTER_STYLE, "symbols", SRC_DESC, "symbols", true),
  A(COUNTER_STYLE, "additiveSymbols", SRC_DESC, "additive-symbols", true),
  A(COUNTER_STYLE, "negative", SRC_DESC, "negative", true),
  A(COUNTER_STYLE, "prefix", SRC_DESC, "prefix", true),
  A(COUNTER_STYLE, "suffix", SRC_DESC, "suffix", true),
  A(COUNTER_STYLE, "range", SRC_DESC, "range", true),
  A(COUNTER_STYLE, "pad", SRC_DESC, "pad", true),
  A(COUNTER_STYLE, "speakAs", SRC_DESC, "speak-as", true),
  A(COUNTER_STYLE, "fallback", SRC_DESC, "fallback", true),
  A(FONT_PALETTE_VALUES, "name", SRC_NAME, NULL, false),
  A(FONT_PALETTE_VALUES, "fontFamily", SRC_DESC, "font-family", false),
  A(FONT_PALETTE_VALUES, "basePalette", SRC_DESC, "base-palette", false),
  A(FONT_PALETTE_VALUES, "overrideColors", SRC_DESC, "override-colors", false),
  A(FONT_FEATURE_VALUES, "fontFamily", SRC_PRELUDE, NULL, true),
  A(VIEW_TRANSITION, "navigation", SRC_DESC, "navigation", false),
  A(VIEW_TRANSITION, "types", SRC_DESC, "types", false),
  A(KEYFRAMES, "name", SRC_NAME, NULL, true),
  A(POSITION_TRY, "name", SRC_NAME, NULL, false),
  A(COLOR_PROFILE, "name", SRC_NAME, NULL, false),
  A(COLOR_PROFILE, "src", SRC_DESC, "src", false),
  A(COLOR_PROFILE, "renderingIntent", SRC_DESC, "rendering-intent", false),
  A(COLOR_PROFILE, "components", SRC_DESC, "components", false),
  A(PAGE_MARGIN, "name", SRC_MARGIN_NAME, NULL, false),
};
#undef A

static const attr_row * attr_find(GTEXT_CSSOM_Rule_Kind kind, const char * name, size_t length) {
  for (size_t i = 0; i < sizeof attr_rows / sizeof attr_rows[0]; i++) {
    const attr_row * r = &attr_rows[i];
    if (r->kind == kind && strlen(r->idl) == length && memcmp(r->idl, name, length) == 0) {
      return r;
    }
  }
  return NULL;
}

/// The top-level pieces of a canonical prelude: runs of text between white space outside quotes and brackets.
typedef struct {
  const char * data;
  size_t length;
} piece;

static size_t split_pieces(const char * s, size_t n, piece * out, size_t cap) {
  size_t count = 0;
  size_t i = 0;
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) {
      i++;
    }
    if (i >= n) {
      break;
    }
    const size_t start = i;
    int depth = 0;
    char quote = 0;
    while (i < n) {
      const char c = s[i];
      if (quote) {
        if (c == '\\' && i + 1 < n) {
          i++;
        }
        else if (c == quote) {
          quote = 0;
        }
      }
      else if (c == '"' || c == '\'') {
        quote = c;
      }
      else if (c == '(' || c == '[') {
        depth++;
      }
      else if (c == ')' || c == ']') {
        depth--;
      }
      else if (depth == 0 && (c == ' ' || c == '\t' || c == '\n')) {
        break;
      }
      i++;
    }
    if (count < cap) {
      out[count].data = s + start;
      out[count].length = i - start;
    }
    count++;
  }
  return count;
}

static bool starts_with_ci(const piece * p, const char * word) {
  const size_t n = strlen(word);
  if (p->length < n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if ((p->data[i] | 32) != (word[i] | 32)) {
      return false;
    }
  }
  return true;
}

/// The inside of `name(...)` or `(...)`, and of a string, written without the quotes and escapes.
static GTEXT_CSS_Status put_unquoted(tbuf * out, const char * s, size_t n) {
  if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
    for (size_t i = 1; i + 1 < n; i++) {
      if (s[i] == '\\' && i + 2 < n) {
        i++;
      }
      if (!tb_put(out, s + i, 1)) {
        return GTEXT_CSS_E_OOM;
      }
    }
    return GTEXT_CSS_OK;
  }
  return tb_put(out, s, n) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

static GTEXT_CSS_Status put_url_argument(tbuf * out, const piece * p) {
  /* url("a.css") or "a.css" */
  const char * s = p->data;
  size_t n = p->length;
  if (n >= 5 && (s[0] | 32) == 'u' && (s[1] | 32) == 'r' && (s[2] | 32) == 'l' && s[3] == '(' && s[n - 1] == ')') {
    s += 4;
    n -= 5;
  }
  return put_unquoted(out, s, n);
}

static GTEXT_CSS_Status attr_value(css_rule * rule, const attr_row * row, tbuf * out, bool * is_null) {
  css_core * core = rule->obj.core;
  *is_null = false;
  if (row->src == SRC_MARGIN_NAME) {
    return tb_put(out, rule->node->name.data, rule->node->name.length) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = css_rule_typed(core, rule, row->src != SRC_DESC, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  size_t pl = 0;
  const char * prelude = gtext_css_atrule_prelude_text(a, &pl);
  piece pieces[16];
  switch (row->src) {
  case SRC_PRELUDE:
    st = tb_put(out, prelude, pl) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    break;
  case SRC_NAME: {
    GTEXT_CSS_String s;
    bool quoted = false;
    if (gtext_css_atrule_name_count(a) > 0 && gtext_css_atrule_name_at(a, 0, &s, &quoted)) {
      st = tb_put(out, s.data, s.length) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    }
    else {
      st = tb_put(out, prelude, pl) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    }
    break;
  }
  case SRC_DESC: {
    GTEXT_CSS_Atrule_Descriptor d;
    if (!gtext_css_atrule_descriptor_find(a, row->desc, strlen(row->desc), &d)) {
      *is_null = strcmp(row->desc, "initial-value") == 0;
      break;   /* absent: the empty string (null for @property's initialValue) */
    }
    st = put_unquoted(out, d.text.data, d.text.length);
    break;
  }
  case SRC_IMPORT_HREF:
  case SRC_IMPORT_LAYER:
  case SRC_IMPORT_SUPPORTS: {
    const size_t n = split_pieces(prelude, pl, pieces, 16);
    if (row->src == SRC_IMPORT_HREF) {
      if (n) {
        st = put_url_argument(out, &pieces[0]);
      }
      break;
    }
    *is_null = true;
    for (size_t i = 1; i < n && i < 16; i++) {
      const bool layer = starts_with_ci(&pieces[i], "layer") && (pieces[i].length == 5 || pieces[i].data[5] == '(');
      const bool supports = starts_with_ci(&pieces[i], "supports(");
      if ((row->src == SRC_IMPORT_LAYER && layer) || (row->src == SRC_IMPORT_SUPPORTS && supports)) {
        *is_null = false;
        const size_t skip = layer ? 5 : 9;
        if (pieces[i].length > skip) {
          if (!tb_put(out, pieces[i].data + skip + (layer ? 1 : 0), pieces[i].length - skip - 1 - (layer ? 1 : 0))) {
            st = GTEXT_CSS_E_OOM;
          }
        }
        break;
      }
    }
    break;
  }
  case SRC_NS_URI:
  case SRC_NS_PREFIX: {
    const size_t n = split_pieces(prelude, pl, pieces, 16);
    const bool has_prefix = n >= 2;
    if (row->src == SRC_NS_PREFIX) {
      if (has_prefix) {
        st = tb_put(out, pieces[0].data, pieces[0].length) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
      }
    }
    else if (n) {
      st = put_url_argument(out, &pieces[has_prefix ? 1 : 0]);
    }
    break;
  }
  case SRC_SCOPE_START:
  case SRC_SCOPE_END: {
    const size_t n = split_pieces(prelude, pl, pieces, 16);
    *is_null = true;
    size_t i = 0;
    if (row->src == SRC_SCOPE_START) {
      if (n && pieces[0].data[0] == '(') {
        *is_null = false;
        st = tb_put(out, pieces[0].data + 1, pieces[0].length - 2) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
      }
      break;
    }
    for (; i + 1 < n; i++) {
      if (pieces[i].length == 2 && (pieces[i].data[0] | 32) == 't' && (pieces[i].data[1] | 32) == 'o' && pieces[i + 1].data[0] == '(') {
        *is_null = false;
        st = tb_put(out, pieces[i + 1].data + 1, pieces[i + 1].length - 2) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
        break;
      }
    }
    break;
  }
  case SRC_CONTAINER_NAME:
  case SRC_CONTAINER_QUERY: {
    const size_t n = split_pieces(prelude, pl, pieces, 16);
    const bool named = n && (pieces[0].data[0] != '(') && !starts_with_ci(&pieces[0], "not") && memchr(pieces[0].data, '(', pieces[0].length) == NULL;
    if (row->src == SRC_CONTAINER_NAME) {
      if (named) {
        st = tb_put(out, pieces[0].data, pieces[0].length) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
      }
    }
    else {
      const size_t skip = named ? 1 : 0;
      if (n > skip) {
        const char * from = pieces[skip].data;
        st = tb_put(out, from, (size_t)(prelude + pl - from)) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
      }
    }
    break;
  }
  default:
    break;
  }
  gtext_css_atrule_free(a);
  return st;
}

GTEXT_CSS_Status gtext_cssom_rule_get_attribute(GTEXT_CSSOM_Rule * rule, const char * name, size_t name_length, char ** out, size_t * length, bool * is_null) {
  if (!rule || !name || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  const attr_row * row = attr_find((GTEXT_CSSOM_Rule_Kind)rule->kind, name, name_length);
  if (!row) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  tbuf b;
  tb_init(&b, &rule->obj.core->alloc);
  bool nul = false;
  GTEXT_CSS_Status st = attr_value(rule, row, &b, &nul);
  if (st != GTEXT_CSS_OK) {
    tb_free(&b);
    return st;
  }
  if (is_null) {
    *is_null = nul;
  }
  return take(&b, out, length);
}

bool gtext_cssom_rule_has_attribute(const GTEXT_CSSOM_Rule * rule, const char * name, size_t name_length, bool * writable) {
  const attr_row * row = rule && name ? attr_find((GTEXT_CSSOM_Rule_Kind)rule->kind, name, name_length) : NULL;
  if (row && writable) {
    *writable = row->writable;
  }
  return row != NULL;
}

GTEXT_CSS_Status gtext_cssom_rule_condition_text(GTEXT_CSSOM_Rule * rule, char ** out, size_t * length) {
  return gtext_cssom_rule_get_attribute(rule, "conditionText", 13, out, length, NULL);
}

typedef struct {
  const char * desc;
  size_t desc_length;
  bool removing;
} desc_check;

/// The new reading holds the descriptor that was set (or none, for a removal).
static GTEXT_CSS_Status desc_present(css_rule * rule, const css_own * own, void * user, bool * ok) {
  const desc_check * c = user;
  GTEXT_CSS_Atrule * a = NULL;
  const GTEXT_CSS_Status st = css_own_typed(rule, own, false, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Atrule_Descriptor d;
  const bool found = a && gtext_css_atrule_descriptor_find(a, c->desc, c->desc_length, &d);
  gtext_css_atrule_free(a);
  *ok = c->removing ? !found : found;
  return GTEXT_CSS_OK;
}

static GTEXT_Markup_Status ident_write(void * user, const char * bytes, size_t n) {
  return tb_put((tbuf *)user, bytes, n) ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_OOM;
}

/// `value` written as an identifier ("serialize an identifier": escaped as needed).
static GTEXT_CSS_Status put_ident(tbuf * out, const char * value, size_t length) {
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = out;
  sink.write = ident_write;
  return css_serialize_identifier_text(value, length, false, &sink);
}

static GTEXT_CSS_Status set_name_like(css_rule * rule, const attr_row * row, const char * value, size_t length, bool * ignored, GTEXT_CSSOM_Outcome * out) {
  css_core * core = rule->obj.core;
  tbuf header, body;
  tb_init(&header, &core->alloc);
  tb_init(&body, &core->alloc);
  GTEXT_CSS_Status st = own_body(rule, &body);
  if (st == GTEXT_CSS_OK) {
    if (row->src == SRC_PRELUDE) {
      /* fontFamily of @font-feature-values: the family list as written */
      if (!tb_puts(&header, "@font-feature-values ") || !tb_put(&header, value, length)) {
        st = GTEXT_CSS_E_OOM;
      }
    }
    else {
      if (!tb_puts(&header, "@") || !tb_put(&header, rule->node->name.data, rule->node->name.length) || !tb_puts(&header, " ")) {
        st = GTEXT_CSS_E_OOM;
      }
      if (st == GTEXT_CSS_OK) {
        /* the name is a string: written as an identifier, escaped; a keyframes name that an identifier cannot be (empty, a reserved word) is a quoted string */
        const bool reserved = length == 0 || (length == 4 && (value[0] | 32) == 'n' && (value[1] | 32) == 'o' && (value[2] | 32) == 'n' && (value[3] | 32) == 'e');
        if (reserved && rule->kind == GTEXT_CSSOM_RULE_KEYFRAMES) {
          if (!tb_puts(&header, "\"") || !tb_put(&header, value, length) || !tb_puts(&header, "\"")) {
            st = GTEXT_CSS_E_OOM;
          }
        }
        else {
          st = put_ident(&header, value, length);
        }
      }
    }
  }
  if (st == GTEXT_CSS_OK) {
    st = css_rule_replace_own(rule, header.data, header.len, body.data, body.len, false, NULL, NULL, ignored, out);
  }
  tb_free(&header);
  tb_free(&body);
  return st;
}

GTEXT_CSS_Status gtext_cssom_rule_set_attribute(GTEXT_CSSOM_Rule * rule, const char * name, size_t name_length, const char * value, size_t value_length,
    GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (!rule || !name || (!value && value_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  const attr_row * row = attr_find((GTEXT_CSSOM_Rule_Kind)rule->kind, name, name_length);
  if (!row) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  GTEXT_CSS_Status st = ready(rule, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!row->writable) {
    out->kind = GTEXT_CSSOM_IGNORED_NOT_ALLOWED_HERE;   /* WebIDL: setting a readonly attribute does nothing */
    out->detail = "the attribute is readonly";
    return GTEXT_CSS_OK;
  }
  if (value_length > rule->obj.core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  bool ignored = false;
  if (row->src == SRC_DESC) {
    css_core * core = rule->obj.core;
    GTEXT_CSS_Atrule * a = NULL;
    st = css_rule_typed(core, rule, false, &a);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    tbuf header, body;
    tb_init(&header, &core->alloc);
    tb_init(&body, &core->alloc);
    st = css_rule_header_text(rule, &header);
    const size_t dl = strlen(row->desc);
    bool placed = false;
    const size_t n = gtext_css_atrule_descriptor_count(a);
    for (size_t i = 0; st == GTEXT_CSS_OK && i < n; i++) {
      GTEXT_CSS_Atrule_Descriptor d;
      gtext_css_atrule_descriptor_at(a, i, &d);
      const bool same = d.name_length == dl && memcmp(d.name, row->desc, dl) == 0;
      if (same && value_length == 0) {
        continue;   /* the empty string removes the descriptor */
      }
      const char * t = same ? value : d.text.data;
      const size_t tl = same ? value_length : d.text.length;
      placed = placed || same;
      if (!tb_put(&body, d.name, d.name_length) || !tb_puts(&body, ": ") || !tb_put(&body, t, tl) || !tb_puts(&body, d.important && !same ? " !important; " : "; ")) {
        st = GTEXT_CSS_E_OOM;
      }
    }
    if (st == GTEXT_CSS_OK && !placed && value_length) {
      if (!tb_put(&body, row->desc, dl) || !tb_puts(&body, ": ") || !tb_put(&body, value, value_length) || !tb_puts(&body, "; ")) {
        st = GTEXT_CSS_E_OOM;
      }
    }
    gtext_css_atrule_free(a);
    if (st == GTEXT_CSS_OK) {
      desc_check dc = {row->desc, dl, value_length == 0};
      st = css_rule_replace_own(rule, header.data, header.len, body.data, body.len, false, desc_present, &dc, &ignored, out);
    }
    tb_free(&header);
    tb_free(&body);
  }
  else {
    st = set_name_like(rule, row, value, value_length, &ignored, out);
  }
  if (st == GTEXT_CSS_OK && ignored) {
    out->kind = GTEXT_CSSOM_IGNORED_INVALID_VALUE;
    out->detail = "the value is not valid for the attribute: nothing changes";
  }
  return st;
}
