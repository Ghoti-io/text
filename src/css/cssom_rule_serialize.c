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
 * `cssText` of every rule and the serialization of a sheet (css_cssom_rule.h, css_cssom_sheet.h): CSSOM's "serialize a CSS rule", one function per kind over the serializers that
 * exist (the selector list, the declaration block, the typed at-rule), and the shape around the rules inside a rule that CSSOM gives (a newline and two spaces before each, a
 * newline and `}` after the last).
 *
 * **No recursion.** The rules inside a rule are written with an explicit stack of frames, so a nest a hundred thousand deep is a status or a long text, never a crash.
 *
 * **The cache.** A rule with no rules inside it keeps its text for as long as its version stays: an unchanged leaf reads in O(1) to reach the buffer. A rule with rules inside
 * does not keep it (its text holds every descendant's, so a nest of depth d would hold O(d x n) bytes), and serializing a sheet or any rule through a sink fills nothing. The
 * cache is bounded by `max_cached_text_bytes`; past it the entries added first are dropped (a clock-free order).
 */

#include <string.h>

#include "css_atrule_internal.h"
#include "css_internal.h"
#include "css_shorthand_internal.h"
#include "css_sheet_internal.h"
#include "cssom_rule_internal.h"

/* ---- the output ---------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  css_core * core;
  const GTEXT_Markup_Sink * sink;
  size_t written;
  GTEXT_CSS_Status st;
} sw;

static void emit(sw * w, const char * s, size_t n) {
  if (w->st != GTEXT_CSS_OK || n == 0) {
    return;
  }
  if (w->written > w->core->config.max_output_bytes || n > w->core->config.max_output_bytes - w->written) {
    w->st = GTEXT_CSS_E_LIMIT_OUTPUT;
    return;
  }
  if (w->sink->write(w->sink->user, s, n) != GTEXT_MARKUP_OK) {
    w->st = GTEXT_CSS_E_WRITE;
    return;
  }
  w->written += n;
}

static void emitz(sw * w, const char * s) {
  emit(w, s, strlen(s));
}

static GTEXT_Markup_Status sw_write(void * user, const char * bytes, size_t n) {
  sw * w = user;
  emit(w, bytes, n);
  return w->st == GTEXT_CSS_OK ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_WRITE;
}

/// A sink that writes into `w`, for the serializers of the layers below.
static GTEXT_Markup_Sink sw_sink(sw * w) {
  GTEXT_Markup_Sink s;
  memset(&s, 0, sizeof s);
  s.user = w;
  s.write = sw_write;
  return s;
}

typedef struct {
  tbuf b;
} collect;

static GTEXT_Markup_Status collect_write(void * user, const char * bytes, size_t n) {
  collect * c = user;
  return tb_put(&c->b, bytes, n) ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_OOM;
}

/* ---- the cache ------------------------------------------------------------------------------------------------------------------------------------ */

void css_rule_drop_cache(css_rule * rule) {
  css_text_cache * c = rule->cache;
  if (!c) {
    return;
  }
  css_core * core = rule->obj.core;
  if (c->prev) {
    c->prev->next = c->next;
  }
  else {
    core->cache_head = c->next;
  }
  if (c->next) {
    c->next->prev = c->prev;
  }
  else {
    core->cache_tail = c->prev;
  }
  const size_t bytes = c->bytes;
  core->cache_bytes -= bytes;
  core->counters.cached_text_bytes = core->cache_bytes;
  core->counters.live_bytes -= bytes;
  rule->cache = NULL;
  GTEXT_Allocator a = core->alloc;
  gtext_allocator_free(&a, c);
}

void css_rule_touch(css_rule * rule) {
  rule->version++;
  css_rule_drop_cache(rule);
  /* its syntax node is no longer its state, nor the node of any rule above it (the invariant: a rule marked has every rule above it marked, so the walk stops at the first) */
  for (css_rule * x = rule; x && !x->mod; x = x->list && x->list->owner->kind == GTEXT_CSSOM_OBJECT_RULE ? (css_rule *)(uintptr_t)x->list->owner : NULL) {
    x->mod = true;
  }
}

/// Keep the text of a leaf rule, if it fits: the entries added first make room, and a text that cannot be kept is not an error.
static void cache_fill(css_rule * rule, const char * text, size_t length) {
  css_core * core = rule->obj.core;
  const size_t bytes = sizeof(css_text_cache) + length + 1;
  if (bytes > core->config.max_cached_text_bytes) {
    return;
  }
  while (core->cache_head && core->cache_bytes + bytes > core->config.max_cached_text_bytes) {
    css_rule_drop_cache(core->cache_head->rule);
  }
  if (css_core_room_bytes(core, bytes) != GTEXT_CSS_OK) {
    return;
  }
  GTEXT_Allocator a = core->alloc;
  css_text_cache * c = gtext_allocator_malloc(&a, bytes);
  if (!c) {
    return;
  }
  c->text = (char *)(c + 1);
  memcpy(c->text, text, length);
  c->text[length] = '\0';
  c->length = length;
  c->version = rule->version;
  c->bytes = bytes;
  c->rule = rule;
  c->next = NULL;
  c->prev = core->cache_tail;
  if (core->cache_tail) {
    core->cache_tail->next = c;
  }
  else {
    core->cache_head = c;
  }
  core->cache_tail = c;
  rule->cache = c;
  core->cache_bytes += bytes;
  core->counters.cached_text_bytes = core->cache_bytes;
  core->counters.live_bytes += bytes;
  core->counters.cache_fills++;
}

/* ---- the pieces ------------------------------------------------------------------------------------------------------------------------------------ */

/// The nearest enclosing style rule or `@scope`, and whether any enclosing rule is a scope: what the selectors of a style rule are read relative to.
static void selector_context(const css_rule * rule, bool * relative, bool * in_scope) {
  *relative = rule->nested;
  *in_scope = false;
  bool nearest_seen = false;
  bool scope_anywhere = false;
  for (const css_rule * x = rule->list && rule->list->owner->kind == GTEXT_CSSOM_OBJECT_RULE ? (const css_rule *)(uintptr_t)rule->list->owner : NULL; x;
      x = x->list && x->list->owner->kind == GTEXT_CSSOM_OBJECT_RULE ? (const css_rule *)(uintptr_t)x->list->owner : NULL) {
    if (x->kind == GTEXT_CSSOM_RULE_STYLE || x->kind == GTEXT_CSSOM_RULE_SCOPE) {
      if (!nearest_seen) {
        *in_scope = x->kind == GTEXT_CSSOM_RULE_SCOPE;
        nearest_seen = true;
      }
      scope_anywhere = scope_anywhere || x->kind == GTEXT_CSSOM_RULE_SCOPE;
    }
  }
  if (!*relative && scope_anywhere) {
    *relative = true;   /* a style rule directly in an `@scope` is read relative to it */
  }
}

/// The selectors of a style rule that stands directly in an `@scope`, group by group. A group is read as a selector, and when it is not one (it begins with a combinator) as a relative
/// selector with the implied `&` taken off again: a browser writes `> a` there, and `a` for `a`.
static GTEXT_CSS_Status write_scope_selectors(sw * w, const css_rule * rule, const GTEXT_CSS_Namespace_Map * map) {
  css_core * core = w->core;
  const GTEXT_CSS_Component_Value * p = rule->node->prelude;
  const size_t n = rule->node->prelude_count;
  GTEXT_CSS_Selector_Options so = gtext_css_selector_options_default();
  const GTEXT_Markup_Sink sink = sw_sink(w);
  size_t gs = 0;
  for (;;) {
    size_t ge = gs;
    while (ge < n && !is_comma(&p[ge])) {
      ge++;
    }
    GTEXT_CSS_Selector_List * list = NULL;
    GTEXT_CSS_Status st = css_sheet_selector_prelude(&core->alloc, &so, map, p + gs, ge - gs, false, false, &list);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (list) {
      st = gtext_css_selector_serialize(list, &sink);
      gtext_css_selector_list_free(list);
      if (st != GTEXT_CSS_OK) {
        return w->st != GTEXT_CSS_OK ? w->st : st;
      }
    }
    else {
      st = css_sheet_selector_prelude(&core->alloc, &so, map, p + gs, ge - gs, true, true, &list);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      if (!list) {
        return GTEXT_CSS_E_INTERNAL;   /* it was valid when the rule was made */
      }
      char * text = NULL;
      size_t length = 0;
      st = gtext_css_selector_serialize_buffer(list, &core->alloc, &text, &length);
      gtext_css_selector_list_free(list);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      const size_t skip = length >= 2 && text[0] == '&' && text[1] == ' ' ? 2 : 0;
      emit(w, text + skip, length - skip);
      gtext_css_selector_buffer_free(&core->alloc, text);
    }
    if (ge >= n) {
      break;
    }
    emitz(w, ", ");
    gs = ge + 1;
  }
  return w->st;
}

/// The serialization of a style rule's selector list, read in the namespaces it was made in.
static GTEXT_CSS_Status write_selectors(sw * w, const css_rule * rule) {
  css_core * core = w->core;
  GTEXT_CSS_Namespace_Map map;
  css_ns_env_map(rule->source ? rule->source->ns : NULL, &map);
  bool relative = false, in_scope = false;
  selector_context(rule, &relative, &in_scope);
  GTEXT_CSS_Selector_Options so = gtext_css_selector_options_default();
  GTEXT_CSS_Selector_List * list = NULL;
  /* inside a style rule the selectors are relative and are written with their `&`; a style rule that is not inside one is read as it is, and in an `@scope` it may begin with a
     combinator, which only a relative selector may (see write_scope_selectors) */
  if (in_scope || (relative && !rule->nested)) {
    /* the nearest rule that makes selectors relative is an `@scope` (whatever is above it): they are written as they are */
    return write_scope_selectors(w, rule, &map);
  }
  const bool nested = rule->nested;
  GTEXT_CSS_Status st = css_sheet_selector_prelude(&core->alloc, &so, &map, rule->node->prelude, rule->node->prelude_count, nested, false, &list);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!list) {
    return GTEXT_CSS_E_INTERNAL;   /* it was valid when the rule was made */
  }
  const GTEXT_Markup_Sink sink = sw_sink(w);
  st = gtext_css_selector_serialize(list, &sink);
  gtext_css_selector_list_free(list);
  return w->st != GTEXT_CSS_OK ? w->st : st;
}

GTEXT_CSS_Status css_rule_selector_list(const css_rule * rule, GTEXT_CSS_Selector_List ** out) {
  css_core * core = rule->obj.core;
  *out = NULL;
  GTEXT_CSS_Namespace_Map map;
  css_ns_env_map(rule->source ? rule->source->ns : NULL, &map);
  bool relative = false, in_scope = false;
  selector_context(rule, &relative, &in_scope);
  const GTEXT_CSS_Selector_Options so = gtext_css_selector_options_default();
  GTEXT_CSS_Selector_List * list = NULL;
  GTEXT_CSS_Status st;
  if (in_scope || (relative && !rule->nested)) {
    /* a style rule directly in an `@scope`: read relative to it */
    st = css_sheet_selector_prelude(&core->alloc, &so, &map, rule->node->prelude, rule->node->prelude_count, true, true, &list);
  }
  else {
    st = css_sheet_selector_prelude(&core->alloc, &so, &map, rule->node->prelude, rule->node->prelude_count, rule->nested, false, &list);
    if (st == GTEXT_CSS_OK && !list) {
      /* a rule that left its parent has lost the context it was read in: read it as relative */
      st = css_sheet_selector_prelude(&core->alloc, &so, &map, rule->node->prelude, rule->node->prelude_count, true, true, &list);
    }
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!list) {
    return GTEXT_CSS_E_INTERNAL;   /* it was valid when the rule was made */
  }
  *out = list;
  return GTEXT_CSS_OK;
}

/// The text of a style rule's or a nested-declarations rule's own declarations, in `out`; empty when there are none.
static GTEXT_CSS_Status declarations_text(css_rule * rule, tbuf * out) {
  css_core * core = rule->obj.core;
  if (rule->style) {
    return css_block_text(rule->style->block, out);
  }
  const GTEXT_CSS_Declaration * decls = NULL;
  size_t n = 0;
  if (!css_rule_declarations(rule, &decls, &n) || n == 0) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Status st = css_decl_block_from_declarations(core, decls, n, false, &block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = css_block_text(block, out);
  gtext_css_block_free(block);
  return st;
}

/// Where the rule stands, for the typed parse of an at-rule (a rule is valid in some places only).
static GTEXT_CSS_Atrule_Parent typed_parent(const css_rule * rule) {
  if (rule->kind == GTEXT_CSSOM_RULE_PAGE_MARGIN) {
    return GTEXT_CSS_ATRULE_PARENT_PAGE;
  }
  if (rule->nested) {
    return GTEXT_CSS_ATRULE_PARENT_STYLE;
  }
  return rule->list && rule->list->owner->kind == GTEXT_CSSOM_OBJECT_RULE ? GTEXT_CSS_ATRULE_PARENT_GROUP : GTEXT_CSS_ATRULE_PARENT_TOP;
}

/// The typed model of an at-rule. `shallow`: without what is inside its block (a grouping rule's header needs the prelude only, and the rules inside are written from the objects).
static GTEXT_CSS_Status typed_rule(css_core * core, const css_rule * rule, bool shallow, GTEXT_CSS_Atrule ** out) {
  GTEXT_CSS_Rule copy = *rule->node;
  if (shallow) {
    copy.contents = NULL;
    copy.content_count = 0;
    copy.rules = NULL;
    copy.rule_count = 0;
    copy.declarations = NULL;
    copy.declaration_count = 0;
  }
  GTEXT_CSS_Atrule_Options o = gtext_css_atrule_options_default();
  o.allocator = &core->alloc;
  o.parent = typed_parent(rule);
  GTEXT_CSS_Atrule_Result res;
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = gtext_css_atrule_parse(core->registry, &copy, &o, &a, &res);
  if (st == GTEXT_CSS_OK && (!a || !res.valid)) {
    gtext_css_atrule_free(a);
    return GTEXT_CSS_E_INTERNAL;   /* it was valid when the rule was made */
  }
  *out = a;
  return st;
}

/// Whether the rule's cssText has a frame of rules inside it: it is a kind with a `cssRules` and has some.
static bool has_children(const css_rule * rule) {
  return rule->rules.count > 0;
}

typedef enum {
  FORM_DONE = 0,   ///< The rule is fully written.
  FORM_KIDS        ///< Its opening is written and the rules inside it follow, and then a newline and `}`.
} form;

/// The prelude of an `@scope` inside a style rule: its root list is relative, so it is written with its `&` (`(& .a) to (.b)`); the limit list is written as it was.
static GTEXT_CSS_Status write_nested_scope_prelude(sw * w, css_rule * rule, bool * handled) {
  *handled = false;
  css_core * core = w->core;
  const GTEXT_CSS_Component_Value * p = rule->node->prelude;
  const size_t n = rule->node->prelude_count;
  size_t i = 0;
  while (i < n && p[i].kind == GTEXT_CSS_VALUE_TOKEN && p[i].type == GTEXT_CSS_TOKEN_WHITESPACE) {
    i++;
  }
  if (i >= n || p[i].kind != GTEXT_CSS_VALUE_BLOCK || p[i].type != GTEXT_CSS_TOKEN_OPEN_PAREN) {
    return GTEXT_CSS_OK;   /* no root list: the prelude is `to (...)`, which is written as it is */
  }
  *handled = true;
  GTEXT_CSS_Namespace_Map map;
  css_ns_env_map(rule->source ? rule->source->ns : NULL, &map);
  GTEXT_CSS_Selector_Options so = gtext_css_selector_options_default();
  GTEXT_CSS_Selector_List * list = NULL;
  GTEXT_CSS_Status st = css_sheet_selector_prelude(&core->alloc, &so, &map, p[i].children, p[i].child_count, true, false, &list);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!list) {
    return GTEXT_CSS_E_INTERNAL;
  }
  const GTEXT_Markup_Sink sink = sw_sink(w);
  emitz(w, "(");
  st = gtext_css_selector_serialize(list, &sink);
  gtext_css_selector_list_free(list);
  emitz(w, ")");
  if (st != GTEXT_CSS_OK) {
    return w->st != GTEXT_CSS_OK ? w->st : st;
  }
  GTEXT_CSS_Serialize_Options vo = gtext_css_serialize_options_default();
  vo.allocator = &core->alloc;
  char * rest = NULL;
  size_t rest_length = 0;
  st = gtext_css_serialize_values_buffer(p + i + 1, n - i - 1, &vo, &rest, &rest_length);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  while (rest_length && (rest[rest_length - 1] == ' ' || rest[rest_length - 1] == '\n' || rest[rest_length - 1] == '\t')) {
    rest_length--;
  }
  emit(w, rest, rest_length);
  gtext_css_serialize_buffer_free(&vo, rest);
  return w->st;
}

/// The `@name prelude {` of a grouping rule. `@media` always has a space before the queries, even when there are none (the list is empty and serializes as nothing).
static GTEXT_CSS_Status write_group_header(sw * w, css_rule * rule) {
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = typed_rule(w->core, rule, true, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  size_t name_length = 0, prelude_length = 0;
  const char * name = gtext_css_atrule_name(a, &name_length);
  const char * prelude = gtext_css_atrule_prelude_text(a, &prelude_length);
  emitz(w, "@");
  emit(w, name, name_length);
  bool root_in_style = false;
  if (rule->kind == GTEXT_CSSOM_RULE_SCOPE && rule->nested) {
    bool relative = false, in_scope = false;
    selector_context(rule, &relative, &in_scope);
    root_in_style = !in_scope;
  }
  bool handled = false;
  if (root_in_style && prelude_length) {
    emitz(w, " ");
    st = write_nested_scope_prelude(w, rule, &handled);
    if (st != GTEXT_CSS_OK) {
      gtext_css_atrule_free(a);
      return st;
    }
  }
  if (!handled && (prelude_length || rule->kind == GTEXT_CSSOM_RULE_MEDIA)) {
    if (!(root_in_style && prelude_length)) {
      emitz(w, " ");
    }
    emit(w, prelude, prelude_length);
  }
  emitz(w, " {");
  gtext_css_atrule_free(a);
  return w->st;
}

/// Write the opening of a rule, and all of it when it holds no rules.
static GTEXT_CSS_Status open_rule(sw * w, css_rule * rule, form * out) {
  css_core * core = w->core;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  *out = FORM_DONE;
  switch ((GTEXT_CSSOM_Rule_Kind)rule->kind) {
  case GTEXT_CSSOM_RULE_STYLE: {
    st = write_selectors(w, rule);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    emitz(w, " {");
    tbuf d;
    tb_init(&d, &core->alloc);
    st = declarations_text(rule, &d);
    if (st == GTEXT_CSS_OK && d.oom) {
      st = GTEXT_CSS_E_OOM;
    }
    if (st == GTEXT_CSS_OK) {
      if (!has_children(rule)) {
        if (d.len) {
          emitz(w, " ");
          emit(w, d.data, d.len);
        }
        emitz(w, " }");
      }
      else {
        if (d.len) {
          emitz(w, "\n  ");
          emit(w, d.data, d.len);
        }
        *out = FORM_KIDS;
      }
    }
    tb_free(&d);
    return st != GTEXT_CSS_OK ? st : w->st;
  }
  case GTEXT_CSSOM_RULE_NESTED_DECLARATIONS: {
    tbuf d;
    tb_init(&d, &core->alloc);
    st = declarations_text(rule, &d);
    if (st == GTEXT_CSS_OK && d.oom) {
      st = GTEXT_CSS_E_OOM;
    }
    if (st == GTEXT_CSS_OK) {
      emit(w, d.data, d.len);
    }
    tb_free(&d);
    return st != GTEXT_CSS_OK ? st : w->st;
  }
  case GTEXT_CSSOM_RULE_MEDIA:
  case GTEXT_CSSOM_RULE_SUPPORTS:
  case GTEXT_CSSOM_RULE_CONTAINER:
  case GTEXT_CSSOM_RULE_STARTING_STYLE:
  case GTEXT_CSSOM_RULE_SCOPE:
  case GTEXT_CSSOM_RULE_WHEN:
  case GTEXT_CSSOM_RULE_ELSE:
    st = write_group_header(w, rule);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (has_children(rule)) {
      *out = FORM_KIDS;
    }
    else {
      emitz(w, "\n}");
    }
    return w->st;
  case GTEXT_CSSOM_RULE_LAYER:
    if (rule->has_block) {
      st = write_group_header(w, rule);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      if (has_children(rule)) {
        *out = FORM_KIDS;
      }
      else {
        emitz(w, "\n}");
      }
      return w->st;
    }
    break;
  case GTEXT_CSSOM_RULE_KEYFRAMES: {
    GTEXT_CSS_Atrule * a = NULL;
    st = typed_rule(core, rule, true, &a);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    const GTEXT_Markup_Sink sink = sw_sink(w);
    st = css_atrule_serialize_part(a, false, &sink);
    gtext_css_atrule_free(a);
    if (st != GTEXT_CSS_OK) {
      return w->st != GTEXT_CSS_OK ? w->st : st;
    }
    emitz(w, " {");
    if (has_children(rule)) {
      *out = FORM_KIDS;
    }
    else {
      emitz(w, " }");
    }
    return w->st;
  }
  case GTEXT_CSSOM_RULE_PAGE: {
    GTEXT_CSS_Atrule * a = NULL;
    st = typed_rule(core, rule, false, &a);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    const GTEXT_Markup_Sink sink = sw_sink(w);
    st = css_atrule_serialize_part(a, false, &sink);
    if (st == GTEXT_CSS_OK) {
      emitz(w, " {");
      collect c;
      tb_init(&c.b, &core->alloc);
      GTEXT_Markup_Sink dsink;
      memset(&dsink, 0, sizeof dsink);
      dsink.user = &c;
      dsink.write = collect_write;
      st = css_atrule_serialize_part(a, true, &dsink);
      if (c.b.oom) {
        st = GTEXT_CSS_E_OOM;
      }
      if (st == GTEXT_CSS_OK) {
        if (!has_children(rule)) {
          if (c.b.len) {
            emitz(w, " ");
            emit(w, c.b.data, c.b.len);
          }
          emitz(w, " }");
        }
        else {
          if (c.b.len) {
            emitz(w, "\n  ");
            emit(w, c.b.data, c.b.len);
          }
          *out = FORM_KIDS;
        }
      }
      tb_free(&c.b);
    }
    gtext_css_atrule_free(a);
    return st != GTEXT_CSS_OK ? st : w->st;
  }
  case GTEXT_CSSOM_RULE_KEYFRAME: {
    GTEXT_CSS_Atrule * a = NULL;
    st = css_atrule_parse_keyframe(core->registry, rule->node, &core->alloc, &a);
    if (st == GTEXT_CSS_OK && !a) {
      st = GTEXT_CSS_E_INTERNAL;
    }
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    const GTEXT_Markup_Sink sink = sw_sink(w);
    st = gtext_css_atrule_serialize(a, &sink);
    gtext_css_atrule_free(a);
    return st != GTEXT_CSS_OK ? st : w->st;
  }
  default:
    break;
  }
  /* every other kind is its typed model's serialization */
  GTEXT_CSS_Atrule * a = NULL;
  st = typed_rule(core, rule, false, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const GTEXT_Markup_Sink sink = sw_sink(w);
  st = gtext_css_atrule_serialize(a, &sink);
  gtext_css_atrule_free(a);
  return st != GTEXT_CSS_OK ? st : w->st;
}

/* ---- the walk ---------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  css_rule * rule;
  css_rule * next;
} sframe;

/// Write a rule and the rules inside it, with an explicit stack.
static GTEXT_CSS_Status write_rule(sw * w, css_rule * top) {
  css_core * core = w->core;
  GTEXT_Allocator a = core->alloc;
  sframe * stack = NULL;
  size_t sp = 0, cap = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  css_rule * pending = top;   /* a rule to open next */
  for (;;) {
    if (pending) {
      form f = FORM_DONE;
      st = open_rule(w, pending, &f);
      if (st != GTEXT_CSS_OK) {
        break;
      }
      if (f == FORM_KIDS) {
        if (sp == cap) {
          sframe * grown = gtext_allocator_realloc(&a, stack, (cap ? cap * 2 : 16) * sizeof *grown);
          if (!grown) {
            st = GTEXT_CSS_E_OOM;
            break;
          }
          stack = grown;
          cap = cap ? cap * 2 : 16;
        }
        stack[sp].rule = pending;
        stack[sp].next = css_rule_list_first(&pending->rules);
        sp++;
      }
      pending = NULL;
      continue;
    }
    if (sp == 0) {
      break;
    }
    sframe * f = &stack[sp - 1];
    if (!f->next) {
      emitz(w, "\n}");
      sp--;
      if (w->st != GTEXT_CSS_OK) {
        st = w->st;
        break;
      }
      continue;
    }
    pending = f->next;
    f->next = css_rule_list_next(pending);
    emitz(w, "\n  ");
  }
  gtext_allocator_free(&a, stack);
  return st != GTEXT_CSS_OK ? st : w->st;
}

/* ---- the public calls --------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_rule_serialize(const GTEXT_CSSOM_Rule * rule, const GTEXT_Markup_Sink * sink) {
  if (!rule || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  css_rule * r = (css_rule *)(uintptr_t)rule;
  sw w = {r->obj.core, sink, 0, GTEXT_CSS_OK};
  if (r->cache && r->cache->version == r->version) {
    emit(&w, r->cache->text, r->cache->length);
    r->obj.core->counters.cache_hits++;
    return w.st;
  }
  return write_rule(&w, r);
}

GTEXT_CSS_Status gtext_cssom_rule_css_text(GTEXT_CSSOM_Rule * rule, char ** out, size_t * length) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = rule->obj.core;
  GTEXT_Allocator a = core->alloc;
  const bool leaf = !has_children(rule);
  if (leaf && rule->cache && rule->cache->version == rule->version) {
    char * copy = css_core_dup(core, rule->cache->text, rule->cache->length);
    if (!copy) {
      return GTEXT_CSS_E_OOM;
    }
    core->counters.cache_hits++;
    *out = copy;
    if (length) {
      *length = rule->cache->length;
    }
    return GTEXT_CSS_OK;
  }
  collect c;
  tb_init(&c.b, &a);
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &c;
  sink.write = collect_write;
  sw w = {core, &sink, 0, GTEXT_CSS_OK};
  GTEXT_CSS_Status st = write_rule(&w, rule);
  if (c.b.oom) {
    st = GTEXT_CSS_E_OOM;   /* the sink that ran out of memory reported a refusal */
  }
  if (st != GTEXT_CSS_OK) {
    tb_free(&c.b);
    return st;
  }
  size_t n = 0;
  char * text = tb_take(&c.b, &n);
  if (!text) {
    return GTEXT_CSS_E_OOM;
  }
  if (leaf) {
    cache_fill(rule, text, n);
  }
  *out = text;
  if (length) {
    *length = n;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_sheet_serialize(const GTEXT_CSSOM_Sheet * sheet, const GTEXT_Markup_Sink * sink) {
  if (!sheet || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!sheet->origin_clean) {
    return GTEXT_CSS_E_SECURITY;
  }
  sw w = {sheet->obj.core, sink, 0, GTEXT_CSS_OK};
  for (css_rule * r = css_rule_list_first(&sheet->rules); r; r = css_rule_list_next(r)) {
    const GTEXT_CSS_Status st = write_rule(&w, r);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    emitz(&w, "\n");
    if (w.st != GTEXT_CSS_OK) {
      return w.st;
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- what the setters and the typed declaration block read ------------------------------------------------------------------------------------ */

GTEXT_CSS_Status css_rule_typed(css_core * core, const css_rule * rule, bool shallow, GTEXT_CSS_Atrule ** out) {
  if (!(css_kind((GTEXT_CSSOM_Rule_Kind)rule->kind)->flags & CSS_KIND_LIST_ANY)) {
    shallow = false;   /* only a rule that has rules inside it can be read without them; `@property` is not valid without its descriptors */
  }
  if (rule->kind == GTEXT_CSSOM_RULE_KEYFRAME) {
    GTEXT_CSS_Atrule * a = NULL;
    GTEXT_CSS_Status st = css_atrule_parse_keyframe(core->registry, rule->node, &core->alloc, &a);
    if (st == GTEXT_CSS_OK && !a) {
      st = GTEXT_CSS_E_INTERNAL;
    }
    *out = a;
    return st;
  }
  return typed_rule(core, rule, shallow, out);
}

GTEXT_CSS_Status css_rule_selector_text(css_rule * rule, tbuf * out) {
  sw w;
  memset(&w, 0, sizeof w);
  w.core = rule->obj.core;
  collect c;
  tb_init(&c.b, &w.core->alloc);
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &c;
  sink.write = collect_write;
  w.sink = &sink;
  w.st = GTEXT_CSS_OK;
  GTEXT_CSS_Status st = write_selectors(&w, rule);
  if (st == GTEXT_CSS_OK && c.b.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK && !tb_put(out, c.b.data, c.b.len)) {
    st = GTEXT_CSS_E_OOM;
  }
  tb_free(&c.b);
  return st;
}
