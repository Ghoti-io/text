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
 * The computed-style call (css_style.h): the element's attributes and the caller's declaration lists, cascaded and computed.
 */

#include "css_anim_internal.h"
#include "css_compute_internal.h"

#include <ghoti.io/text/css/css_presentation.h>
#include <ghoti.io/text/css/css_style.h>

#include <string.h>

/** The most diagnostics a style keeps unless the options say another number (candidates.max_diagnostics); the rest are counted. */
#define MAX_KEPT_DIAGNOSTICS 1024u

GTEXT_CSS_Style_Options gtext_css_style_options_default(void) {
  GTEXT_CSS_Style_Options o;
  memset(&o, 0, sizeof o);
  o.sources = GTEXT_CSS_STYLE_PRESENTATION_ATTRIBUTES | GTEXT_CSS_STYLE_STYLE_ATTRIBUTE;
  o.presentation = gtext_css_presentation_options_default();
  o.candidates = gtext_css_candidate_options_default();
  o.parse = gtext_css_parse_options_default();
  return o;
}

/* ---- one computation ---------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_CSS_Compute_Context * cc;
  const GTEXT_CSS_Style_Options * options;
  GTEXT_CSS_Compute_Result * res;
  GTEXT_CSS_Candidates * set;
  css_style_diag * diag;
  size_t ndiag, capdiag, diag_total;
  uint32_t ordinal; ///< The next `declaration` key.
  size_t work;
} build;

static GTEXT_CSS_Status fail(build * b, GTEXT_CSS_Status status, const char * name, size_t name_length, const char * detail) {
  GTEXT_CSS_Compute_Result * r = b->res;
  if (r) {
    r->status = status;
    r->message = gtext_css_status_string(status);
    if (name) {
      char tmp[96];
      size_t n = name_length < sizeof tmp - 1 ? name_length : sizeof tmp - 1;
      memcpy(tmp, name, n);
      tmp[n] = '\0';
      css_copy_name(r->name, sizeof r->name, tmp);
    }
    r->detail = detail;
    r->work = b->work;
  }
  return status;
}

static GTEXT_CSS_Status fail_cascade(build * b, GTEXT_CSS_Status status, const GTEXT_CSS_Cascade_Result * cr) {
  if (b->res) {
    b->res->status = status;
    b->res->message = gtext_css_status_string(status);
    if (cr) {
      css_copy_name(b->res->name, sizeof b->res->name, cr->name);
      b->res->property = cr->property;
      b->res->detail = cr->detail;
    }
    b->res->work = b->work;
  }
  return status;
}

static GTEXT_CSS_Status add_diag(build * b, GTEXT_CSS_Style_Diagnostic_Kind kind, GTEXT_CSS_Style_Source source, size_t entry, const char * name,
    size_t name_length, size_t offset, size_t length, size_t line, size_t column) {
  b->diag_total++;
  /* the bound of the candidate options is the style's too: more are counted and not kept */
  size_t cap = b->options->candidates.max_diagnostics ? b->options->candidates.max_diagnostics : MAX_KEPT_DIAGNOSTICS;
  if (cap > ((size_t)1 << 20)) {
    cap = (size_t)1 << 20;
  }
  if (b->ndiag >= cap) {
    return GTEXT_CSS_OK;
  }
  GTEXT_Allocator a = b->cc->alloc;
  if (b->ndiag == b->capdiag) {
    const size_t cap = b->capdiag ? b->capdiag * 2 : 8;
    css_style_diag * grown = gtext_allocator_realloc(&a, b->diag, cap * sizeof *grown);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    b->diag = grown;
    b->capdiag = cap;
  }
  char * copy = gtext_allocator_malloc(&a, name_length + 1);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(copy, name, name_length);
  copy[name_length] = '\0';
  css_style_diag * d = &b->diag[b->ndiag++];
  d->kind = (int)kind;
  d->source = (int)source;
  d->entry = entry;
  d->name_copy = copy;
  d->name_length = name_length;
  d->offset = offset;
  d->length = length;
  d->line = line;
  d->column = column;
  return GTEXT_CSS_OK;
}

/** Add the declarations of a list (a `style` attribute, the body of a rule). */
static GTEXT_CSS_Status add_list(build * b, const GTEXT_CSS_Cascade_Entry * base, const char * text, size_t length, size_t base_offset,
    GTEXT_CSS_Style_Source source, size_t entry) {
  GTEXT_CSS_Parse_Options po = b->options->parse;
  if (!po.allocator) {
    po.allocator = &b->cc->alloc;
  }
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result pr;
  memset(&pr, 0, sizeof pr);
  GTEXT_CSS_Status st = gtext_css_parse_declaration_list(text, length, &po, &tree, &pr);
  if (st != GTEXT_CSS_OK) {
    const GTEXT_CSS_Status s2 = fail(b, st, NULL, 0, "the declaration list could not be read (a limit of the parse)");
    gtext_css_result_free(&pr);
    return s2;
  }
  b->work += length / 8 + tree->declaration_count;
  GTEXT_CSS_Property_Options propo;
  memset(&propo, 0, sizeof propo);
  propo.quirks = b->options->quirks;
  for (size_t i = 0; i < tree->declaration_count && st == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Declaration * d = &tree->declarations[i];
    GTEXT_CSS_Cascade_Entry k = *base;
    k.declaration = b->ordinal++;
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    GTEXT_CSS_Cascade_Result cr;
    st = gtext_css_candidates_add_declaration(b->set, &k, d, &propo, &outcome, &cr);
    GTEXT_CSS_Style_Diagnostic_Kind kind = GTEXT_CSS_STYLE_DIAG_INVALID_VALUE;
    bool drop = false;
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      kind = GTEXT_CSS_STYLE_DIAG_UNJUDGEABLE;
      drop = true;
      st = GTEXT_CSS_OK;
    }
    else if (st != GTEXT_CSS_OK) {
      st = fail_cascade(b, st, &cr);
      break;
    }
    else if (outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN) {
      kind = GTEXT_CSS_STYLE_DIAG_UNKNOWN_PROPERTY;
      drop = true;
    }
    else if (outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
      drop = true;
    }
    if (drop) {
      st = add_diag(b, kind, source, entry, d->name.data, d->name.length, base_offset + d->span.offset, d->span.end - d->span.offset, d->span.line,
          d->span.column);
      if (st != GTEXT_CSS_OK) {
        fail(b, st, NULL, 0, NULL);
      }
    }
  }
  gtext_css_tree_free(tree);
  gtext_css_result_free(&pr);
  return st;
}

/** Read an attribute through the table of the caller's element interface. */
static GTEXT_CSS_Status read_attribute(build * b, const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element, const char * name,
    size_t name_length, GTEXT_CSS_String * out, bool * found) {
  GTEXT_CSS_String n = {name, name_length};
  GTEXT_CSS_Element_Result er;
  const GTEXT_CSS_Status st = gtext_css_element_get_attribute(iface, element, NULL, &n, out, found, &er);
  if (st != GTEXT_CSS_OK) {
    return fail(b, st, er.callback ? er.callback : "attribute", er.callback ? strlen(er.callback) : 9, "the element interface lacks a callback the style needs");
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status add_presentation_attributes(build * b, const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle element) {
  const GTEXT_CSS_Presentation_Options * po = &b->options->presentation;
  const GTEXT_CSS_Presentation_Table * table = po->table ? po->table : gtext_css_presentation_table_svg2();
  GTEXT_CSS_Element_Result er;
  GTEXT_CSS_String local = {NULL, 0}, ns = {NULL, 0};
  bool have_local = false, have_ns = false;
  GTEXT_CSS_Status st = gtext_css_element_get_local_name(iface, element, &local, &have_local, &er);
  if (st != GTEXT_CSS_OK) {
    return fail(b, st, er.callback ? er.callback : "local_name", er.callback ? strlen(er.callback) : 10, "the presentation attributes need the element's name");
  }
  st = gtext_css_element_get_namespace_uri(iface, element, &ns, &have_ns, &er);
  if (st != GTEXT_CSS_OK) {
    return fail(b, st, er.callback ? er.callback : "namespace_uri", er.callback ? strlen(er.callback) : 13, "the presentation attributes need the element's namespace");
  }
  if (!have_local) {
    return GTEXT_CSS_OK; /* an element with no name has no attributes of the table */
  }
  /* the strings of the interface are valid until the next call of a callback: copy the name and the namespace */
  char lbuf[128], nbuf[128];
  if (local.length >= sizeof lbuf || (have_ns && ns.length >= sizeof nbuf)) {
    return GTEXT_CSS_OK; /* no element of a table has a name this long */
  }
  memcpy(lbuf, local.data, local.length);
  const size_t ll = local.length;
  size_t nl = 0;
  if (have_ns) {
    memcpy(nbuf, ns.data, ns.length);
    nl = ns.length;
  }
  for (size_t i = 0; i < table->count; i++) {
    const GTEXT_CSS_Presentation_Row * row = &table->rows[i];
    if (!css_presentation_applies(row, have_ns ? nbuf : NULL, nl, lbuf, ll)) {
      continue;
    }
    GTEXT_CSS_String value = {NULL, 0};
    bool found = false;
    st = read_attribute(b, iface, element, row->attribute, strlen(row->attribute), &value, &found);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (!found) {
      continue;
    }
    GTEXT_CSS_Cascade_Entry keys;
    memset(&keys, 0, sizeof keys);
    keys.declaration = b->ordinal++;
    GTEXT_CSS_Presentation_Diagnostic pd;
    GTEXT_CSS_Cascade_Result cr;
    b->work += value.length / 8 + 1;
    st = gtext_css_candidates_add_presentation(b->set, &keys, row, value.data, value.length, po, &pd, &cr);
    if (st != GTEXT_CSS_OK) {
      return fail_cascade(b, st, &cr);
    }
    if (pd.outcome != GTEXT_CSS_PRESENTATION_OK) {
      GTEXT_CSS_Style_Diagnostic_Kind kind = GTEXT_CSS_STYLE_DIAG_INVALID_VALUE;
      if (pd.outcome == GTEXT_CSS_PRESENTATION_UNKNOWN_PROPERTY) {
        kind = GTEXT_CSS_STYLE_DIAG_UNKNOWN_PROPERTY;
      }
      else if (pd.outcome == GTEXT_CSS_PRESENTATION_UNJUDGEABLE) {
        kind = GTEXT_CSS_STYLE_DIAG_UNJUDGEABLE;
      }
      st = add_diag(b, kind, GTEXT_CSS_STYLE_SOURCE_PRESENTATION, 0, row->attribute, strlen(row->attribute), pd.offset, 0, 0, 0);
      if (st != GTEXT_CSS_OK) {
        return fail(b, st, NULL, 0, NULL);
      }
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- highlight inheritance ---------------------------------------------------------------------------- */

/* CSS Pseudo-Elements 4, "Styling Highlights": "The following properties apply to the highlight pseudo-elements: 'color', 'background-color', 'text-decoration' and its associated properties
   (including 'text-underline-position' and 'text-underline-offset'), 'text-shadow', 'stroke-color', 'fill-color', and 'stroke-width', custom properties". */
static const char * const HIGHLIGHT_PROPERTIES[] = {"color", "background-color", "text-decoration-line", "text-decoration-style", "text-decoration-color", "text-decoration-thickness",
    "text-decoration-skip-ink", "text-decoration-trim", "text-underline-position", "text-underline-offset", "text-shadow", "stroke-color", "fill-color", "stroke-width"};

/// Whether the cascade gave the property a value that is its own: "When any supported property is not given a value by the cascade, or given a value of inherit or unset, its specified value is
/// determined by inheritance from the corresponding highlight pseudo-element of its originating element's parent element." For `color` the keyword `currentcolor` is `inherit` (Color 4, 4.4:
/// "In the color property, the used value of currentcolor is the resolved inherited value"), and what is inherited here is the highlight's.
static bool highlight_given(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id id, bool is_color) {
  GTEXT_CSS_Declaration_Item it;
  if (!block || !gtext_css_block_find(block, id, NULL, 0, &it)) {
    return false;
  }
  if (it.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    return it.wide != GTEXT_CSS_WIDE_INHERIT && it.wide != GTEXT_CSS_WIDE_UNSET;
  }
  if (is_color && it.outcome == GTEXT_CSS_PROP_RESULT_VALID && it.set) {
    const GTEXT_CSS_Value * v = gtext_css_value_set_root(it.set);
    const GTEXT_CSS_Color * c = (v && v->kind == GTEXT_CSS_VAL_COLOR) ? v->color : NULL;
    for (size_t guard = 0; c && c->kind == GTEXT_CSS_COLOR_LIGHT_DARK && c->item_count == 2 && c->items[0] && c->items[1] && guard < 100000; guard++) {
      c = c->items[0];   /* either branch: a currentcolor in one and not the other is not told apart here, and the first is taken */
    }
    if (c && c->kind == GTEXT_CSS_COLOR_KEYWORD && c->keyword == GTEXT_CSS_COLOR_KW_CURRENTCOLOR) {
      return false;
    }
  }
  return true;
}

/// The computed value of `color` that is the keyword `currentcolor`: the inherited value of `color` of a highlight of the root element, which stands for the originating element's color.
static GTEXT_CSS_Status highlight_current_color(const GTEXT_CSS_Compute_Context * cc, GTEXT_CSS_Property_Id color, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &cc->alloc;
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  const css_pmeta * m = &cc->meta[color - 1];
  GTEXT_CSS_Status st = gtext_css_parse_property_text(cc->registry, m->name, m->name_length, "currentcolor", 12, &po, &pv);
  if (st == GTEXT_CSS_OK && (pv.outcome != GTEXT_CSS_PROP_RESULT_VALID || !gtext_css_value_set_root(pv.set))) {
    st = GTEXT_CSS_E_UNSUPPORTED;
  }
  if (st == GTEXT_CSS_OK) {
    *out = gtext_css_value_set_retain(pv.set);
  }
  gtext_css_property_value_free(&pv);
  return st;
}

/// Highlight inheritance (css_style.h, GTEXT_CSS_Style_Options::highlight): the supported properties the cascade gave no value take the parent highlight's, or at the root the initial value.
static GTEXT_CSS_Status highlight_inherit(const GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Computed_Style * style,
    const GTEXT_CSS_Computed_Style * parent, const GTEXT_CSS_Computed_Style * origin) {
  const GTEXT_CSS_Property_Id color = cc->pid[CSS_P_COLOR];
  GTEXT_CSS_Property_Id supported[sizeof HIGHLIGHT_PROPERTIES / sizeof *HIGHLIGHT_PROPERTIES];
  for (size_t k = 0; k < sizeof supported / sizeof *supported; k++) {
    supported[k] = gtext_css_registry_find_property(cc->registry, HIGHLIGHT_PROPERTIES[k], strlen(HIGHLIGHT_PROPERTIES[k]));
  }
  if (origin) {
    /* "For any properties not listed above, but which are required to resolve the values of applicable properties, their computed values are copied from those of the originating
       element, ignoring any values specified on the highlight pseudo-element itself." (Custom properties are on the list, and keep what the cascade made of them.) */
    for (GTEXT_CSS_Property_Id id = 1; id <= cc->nprops && id <= origin->n && id <= style->n; id++) {
      bool listed = false;
      for (size_t k = 0; k < sizeof supported / sizeof *supported; k++) {
        listed = listed || supported[k] == id;
      }
      if (!listed) {
        GTEXT_CSS_Value_Set * copy = gtext_css_value_set_retain(origin->vals[id - 1]);
        gtext_css_value_set_free(style->vals[id - 1]);
        style->vals[id - 1] = copy;
      }
    }
    style->flow = origin->flow;
  }
  for (size_t k = 0; k < sizeof HIGHLIGHT_PROPERTIES / sizeof *HIGHLIGHT_PROPERTIES; k++) {
    const char * name = HIGHLIGHT_PROPERTIES[k];
    const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(cc->registry, name, strlen(name));
    if (!id || id > cc->nprops || highlight_given(block, id, id == color)) {
      continue;
    }
    GTEXT_CSS_Value_Set * set = NULL;
    if (parent) {
      set = gtext_css_value_set_retain(parent->vals[id - 1]);
    }
    else if (id == color) {
      const GTEXT_CSS_Status st = highlight_current_color(cc, id, &set);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
    }
    else if (cc->meta[id - 1].initial && !cc->meta[id - 1].initial_dependent) {
      set = gtext_css_value_set_retain(cc->meta[id - 1].initial);
    }
    else {
      continue;   /* an initial value that depends on the element: the style's own, computed without a parent value, stands */
    }
    gtext_css_value_set_free(style->vals[id - 1]);
    style->vals[id - 1] = set;
  }
  return GTEXT_CSS_OK;
}

/// Whether the pseudo-element list is `::before` or `::after` (the single-colon spelling reads as the same simple selector).
static bool is_before_after(const GTEXT_CSS_Selector_List * pseudo) {
  if (!pseudo || pseudo->count != 1 || pseudo->complex[0].compound_count != 1 || pseudo->complex[0].compounds[0].simple_count != 1) {
    return false;
  }
  const GTEXT_CSS_Simple * s = &pseudo->complex[0].compounds[0].simples[0];
  if (s->kind != GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT) {
    return false;
  }
  return (s->name.length == 6 && memcmp(s->name.data, "before", 6) == 0) || (s->name.length == 5 && memcmp(s->name.data, "after", 5) == 0);
}

/// The computed value of `content` of ::before and ::after that is `normal` is `none`: both browsers answer so (css/css-content/computed-value.html: "initial computed value of content
/// for elements is not none", where the pseudo-element's is).
static GTEXT_CSS_Status content_normal_is_none(const GTEXT_CSS_Compute_Context * cc, GTEXT_CSS_Computed_Style * style) {
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(cc->registry, "content", 7);
  if (!id || id > cc->nprops || id > style->n || !css_val_is_keyword(gtext_css_value_set_root(style->vals[id - 1]), "normal")) {
    return GTEXT_CSS_OK;
  }
  if (!cc->content_none) {
    return GTEXT_CSS_OK;
  }
  gtext_css_value_set_free(style->vals[id - 1]);
  style->vals[id - 1] = gtext_css_value_set_retain(cc->content_none);
  return GTEXT_CSS_OK;
}

/// The candidates of one element: the style rules that match it, the presentation attributes, the `style` attribute and the caller's entries. `*early` is set when a failure was reported
/// and the set is already freed (b->set is then NULL); on another failure the set is left for the caller to free.
static GTEXT_CSS_Status fill_candidates(build * b, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Element_Interface * iface, const GTEXT_CSS_Style_Entry * entries, size_t count,
    bool * early) {
  const GTEXT_CSS_Compute_Context * context = b->cc;
  const GTEXT_CSS_Style_Options * options = b->options;
  *early = false;
  GTEXT_CSS_Candidate_Options co = options->candidates;
  if (!co.allocator) {
    co.allocator = &context->alloc;
  }
  GTEXT_CSS_Status st = gtext_css_candidates_new(context->registry, options->layers, &co, &b->set);
  if (st != GTEXT_CSS_OK) {
    *early = true;
    return fail(b, st, NULL, 0, NULL);
  }
  if (options->rules) {
    GTEXT_CSS_Rule_Index_Result rr;
    if (!options->match || !element) {
      gtext_css_candidates_free(b->set);
      b->set = NULL;
      *early = true;
      return fail(b, GTEXT_CSS_E_INVALID, NULL, 0, "style rules are given with no match context or no element");
    }
    GTEXT_CSS_Rules_Collect_Options ro;
    memset(&ro, 0, sizeof ro);
    ro.starting_style = options->starting_style;
    ro.call = options->container_call;
    st = gtext_css_rules_collect(options->rules, options->match, element, options->pseudo, b->set, &ro, &rr);
    b->work += rr.work;
    if (st != GTEXT_CSS_OK) {
      gtext_css_candidates_free(b->set);
      b->set = NULL;
      *early = true;
      return fail(b, st, rr.name, strlen(rr.name), rr.detail);
    }
  }
  if (options->sources & GTEXT_CSS_STYLE_PRESENTATION_ATTRIBUTES) {
    st = add_presentation_attributes(b, iface, element);
  }
  if (st == GTEXT_CSS_OK && (options->sources & GTEXT_CSS_STYLE_STYLE_ATTRIBUTE)) {
    GTEXT_CSS_String value = {NULL, 0};
    bool found = false;
    st = read_attribute(b, iface, element, "style", 5, &value, &found);
    if (st == GTEXT_CSS_OK && found) {
      GTEXT_CSS_Cascade_Entry keys;
      memset(&keys, 0, sizeof keys);
      keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
      keys.element_attached = true;
      st = add_list(b, &keys, value.data, value.length, options->style_attribute_offset, GTEXT_CSS_STYLE_SOURCE_STYLE_ATTRIBUTE, 0);
    }
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < count; i++) {
    GTEXT_CSS_Cascade_Entry keys = entries[i].keys;
    keys.presentational_hint = false;
    keys.element_attached = false;
    st = add_list(b, &keys, entries[i].text.data ? entries[i].text.data : "", entries[i].text.length, entries[i].base_offset, GTEXT_CSS_STYLE_SOURCE_ENTRY, i);
  }
  return st;
}

/// `revert` in a keyframe (css_anim_internal.h, css_anim_rollback): the cascade of the same element again, with `name: revert` declared in the animation origin on top of everything, which the
/// cascade rolls back to the value of the user and user-agent origins by its own rules (css_revert.c). The first cascade is sealed, so the candidates are collected anew; this is asked
/// only when a keyframe holds a `revert`, once for each name in it.
typedef struct {
  const GTEXT_CSS_Compute_Context * cc;
  const GTEXT_CSS_Style_Options * options;
  GTEXT_CSS_Element_Handle element;
  const GTEXT_CSS_Element_Interface * iface;
  const GTEXT_CSS_Computed_Style * parent;
  const GTEXT_CSS_Style_Entry * entries;
  size_t count;
} style_roll;

static GTEXT_CSS_Status style_roll_back(void * user, const char * name, size_t name_length, GTEXT_CSS_Declaration_Block ** out) {
  const style_roll * r = user;
  build rb;
  memset(&rb, 0, sizeof rb);
  rb.cc = r->cc;
  rb.options = r->options;
  bool early = false;
  GTEXT_CSS_Status st = fill_candidates(&rb, r->element, r->iface, r->entries, r->count, &early);
  GTEXT_CSS_Tree * tree = NULL;
  char * text = NULL;
  if (st == GTEXT_CSS_OK) {
    text = gtext_allocator_malloc(&r->cc->alloc, name_length + 9);
    if (!text) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      memcpy(text, name, name_length);
      memcpy(text + name_length, ": revert", 8);
      GTEXT_CSS_Parse_Options po = r->options->parse;
      if (!po.allocator) {
        po.allocator = &r->cc->alloc;
      }
      st = gtext_css_parse_declaration_list(text, name_length + 8, &po, &tree, NULL);
    }
  }
  if (st == GTEXT_CSS_OK && tree->declaration_count >= 1) {
    GTEXT_CSS_Cascade_Entry k;
    memset(&k, 0, sizeof k);
    k.origin = GTEXT_CSS_ORIGIN_ANIMATION;
    k.declaration = rb.ordinal++;
    GTEXT_CSS_Property_Options propo;
    memset(&propo, 0, sizeof propo);
    propo.quirks = r->options->quirks;
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    GTEXT_CSS_Cascade_Result cr;
    st = gtext_css_candidates_add_declaration(rb.set, &k, &tree->declarations[0], &propo, &outcome, &cr);
  }
  GTEXT_CSS_Cascaded * cascaded = NULL;
  if (st == GTEXT_CSS_OK) {
    GTEXT_CSS_Cascade_Options cascade;
    memset(&cascade, 0, sizeof cascade);
    cascade.allocator = &r->cc->alloc;
    GTEXT_CSS_Flow pf;
    if (r->parent) {
      pf = gtext_css_computed_flow(r->parent);
      cascade.parent_flow = &pf;
    }
    cascade.hash_seed = r->cc->options.custom_hash_seed;
    GTEXT_CSS_Cascade_Result cr;
    st = gtext_css_cascade(rb.set, &cascade, &cascaded, &cr);
  }
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_cascaded_to_block(cascaded, NULL, out);
  }
  gtext_css_cascaded_free(cascaded);
  gtext_css_tree_free(tree);
  gtext_allocator_free(&r->cc->alloc, text);
  if (!early) {
    gtext_css_candidates_free(rb.set);
  }
  for (size_t i = 0; i < rb.ndiag; i++) {
    gtext_allocator_free(&r->cc->alloc, rb.diag[i].name_copy);
  }
  gtext_allocator_free(&r->cc->alloc, rb.diag);
  return st;
}

/// The candidates of the set that a pass replaced with a rolling keyword for the next (a declaration whose functions were replaced and came to `revert`, `revert-layer` or
/// `revert-rule`: the cascade was over when that was known, so the cascade is run again with the declaration replaced by the keyword, which it rolls back from by its own
/// rules, and the element is computed again). The index is the candidate's place in the set, which collecting the same sources again gives the same.
typedef struct {
  size_t index;
  GTEXT_CSS_Wide_Keyword kind;
} style_pick;

typedef struct {
  style_pick * v;
  size_t n, cap;
} style_picks;

/// How many times a computation is made again for the declarations that roll back (each pass takes one more step down the layers a chain of them goes through).
#define MAX_ROLL_PASSES 8u

/// The picks the requests of a pass add: `*added` says whether any is new.
static GTEXT_CSS_Status style_add_picks(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Cascaded * cascaded, const css_roll_set * roll, style_picks * picks, bool * added) {
  *added = false;
  GTEXT_Allocator a = context->alloc;
  for (size_t i = 0; i < roll->n; i++) {
    GTEXT_CSS_Cascade_Entry entry;
    if (!gtext_css_cascaded_find(cascaded, roll->v[i].property, roll->v[i].name, roll->v[i].name_length, &entry)) {
      continue;
    }
    bool have = false;
    for (size_t k = 0; k < picks->n && !have; k++) {
      have = picks->v[k].index == entry.index;
    }
    if (have) {
      continue;
    }
    if (picks->n == picks->cap) {
      const size_t cap = picks->cap ? picks->cap * 2 : 4;
      style_pick * grown = gtext_allocator_realloc(&a, picks->v, cap * sizeof *grown);
      if (!grown) {
        return GTEXT_CSS_E_OOM;
      }
      picks->v = grown;
      picks->cap = cap;
    }
    picks->v[picks->n++] = (style_pick){entry.index, roll->v[i].kind};
    *added = true;
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status style_compute_pass(const GTEXT_CSS_Compute_Context * context, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Computed_Style * parent_style, const GTEXT_CSS_Style_Entry * entries, size_t count, const GTEXT_CSS_Style_Options * options,
    style_picks * picks, bool final, bool * again, GTEXT_CSS_Computed_Style ** out, GTEXT_CSS_Compute_Result * result) {
  *again = false;
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  GTEXT_CSS_Style_Options def;
  if (!options) {
    def = gtext_css_style_options_default();
    options = &def;
  }
  build b;
  memset(&b, 0, sizeof b);
  b.cc = context;
  b.options = options;
  b.res = result;
  if (!context || !out || (!entries && count)) {
    return fail(&b, GTEXT_CSS_E_INVALID, NULL, 0, "a NULL argument");
  }
  for (size_t i = 0; i < count; i++) {
    if (!entries[i].text.data && entries[i].text.length) {
      return fail(&b, GTEXT_CSS_E_INVALID, NULL, 0, "an entry has a length and no text");
    }
  }
  if (parent_style && parent_style->cc != context) {
    return fail(&b, GTEXT_CSS_E_INVALID, NULL, 0, "the parent style was made for another context");
  }
  if (options->highlight_parent && (options->highlight_parent->cc != context || !options->highlight)) {
    return fail(&b, GTEXT_CSS_E_INVALID, NULL, 0, "the highlight parent was made for another context, or the style is not a highlight's");
  }
  if (options->sources != 0 && !element) {
    return fail(&b, GTEXT_CSS_E_INVALID, NULL, 0, "attributes are read, and there is no element");
  }
  const GTEXT_CSS_Element_Interface * iface = context->has_iface ? &context->iface : NULL;
  if (options->sources != 0 && !iface) {
    return fail(&b, GTEXT_CSS_E_UNSUPPORTED, "attribute", 9, "the context has no element interface to read attributes through");
  }
  bool early = false;
  GTEXT_CSS_Status st = fill_candidates(&b, element, iface, entries, count, &early);
  if (early) {
    return st;
  }
  GTEXT_CSS_Cascaded * cascaded = NULL;
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Computed_Style * style = NULL;
  for (size_t i = 0; st == GTEXT_CSS_OK && i < picks->n; i++) {
    /* a declaration that an earlier pass found to be a rolling keyword once its functions were replaced */
    st = css_candidates_roll(b.set, picks->v[i].index, picks->v[i].kind);
    if (st != GTEXT_CSS_OK) {
      fail(&b, st, NULL, 0, "a declaration could not be replaced by the keyword it came to");
    }
  }
  if (st == GTEXT_CSS_OK) {
    GTEXT_CSS_Cascade_Options cascade;
    memset(&cascade, 0, sizeof cascade);
    cascade.allocator = &context->alloc;
    GTEXT_CSS_Flow pf;
    if (parent_style) {
      pf = gtext_css_computed_flow(parent_style);
      cascade.parent_flow = &pf;
    }
    cascade.hash_seed = context->options.custom_hash_seed;
    GTEXT_CSS_Cascade_Result cr;
    st = gtext_css_cascade(b.set, &cascade, &cascaded, &cr);
    b.work += cr.work + gtext_css_candidates_count(b.set);
    if (st != GTEXT_CSS_OK) {
      fail_cascade(&b, st, &cr);
    }
  }
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_cascaded_to_block(cascaded, NULL, &block);
    if (st != GTEXT_CSS_OK) {
      fail(&b, st, NULL, 0, "the winners could not be made a block");
    }
  }
  css_anim_untimed untimed;
  memset(&untimed, 0, sizeof untimed);
  if (st == GTEXT_CSS_OK && options->animation) {
    /* the animations of the element, sampled at the times the caller gave: their values are declared in the animation origin, over the winners of the cascade */
    GTEXT_CSS_Declaration_Block * animated = NULL;
    GTEXT_CSS_Compute_Result sampled;
    memset(&sampled, 0, sizeof sampled);
    style_roll roll = {context, options, element, iface, parent_style, entries, count};
    css_anim_rollback rollback = {style_roll_back, &roll};
    st = css_anim_sample(context, options, element, parent_style, block, &rollback, &animated, &untimed, &sampled);
    if (st != GTEXT_CSS_OK) {
      if (result && sampled.status == st) {
        *result = sampled;   /* the compute of a keyframe failed: it says which callback or property it lacked */
      }
      else {
        fail(&b, st, NULL, 0, "the animations of the element could not be sampled");
      }
    }
    else if (animated) {
      gtext_css_block_free(block);
      block = animated;
    }
  }
  css_roll_set roll;
  memset(&roll, 0, sizeof roll);
  if (st == GTEXT_CSS_OK) {
    const size_t before = b.work;
    GTEXT_CSS_Compute_Result cres;
    st = css_compute_style_rolling(context, block, parent_style, element, final ? NULL : &roll, options->pseudo != NULL, &style, &cres);
    if (result) {
      *result = cres;
      result->work += before;
    }
    else {
      (void)before;
    }
  }
  if (st == GTEXT_CSS_OK && roll.n) {
    /* the declarations that rolled back: the cascade is run again without them, and this style, made with them invalid, is dropped */
    bool added = false;
    st = style_add_picks(context, cascaded, &roll, picks, &added);
    if (st != GTEXT_CSS_OK) {
      fail(&b, st, NULL, 0, "the declarations to roll back could not be listed");
    }
    else if (added) {
      *again = true;
      gtext_css_computed_free(style);
      style = NULL;
    }
  }
  css_roll_set_free(&context->alloc, &roll);
  if (st == GTEXT_CSS_OK && !*again && is_before_after(options->pseudo)) {
    st = content_normal_is_none(context, style);
    if (st != GTEXT_CSS_OK) {
      gtext_css_computed_free(style);
      style = NULL;
      fail(&b, st, NULL, 0, "the computed content could not be made");
    }
  }
  if (st == GTEXT_CSS_OK && !*again && options->highlight) {
    st = highlight_inherit(context, block, style, options->highlight_parent, parent_style);
    if (st != GTEXT_CSS_OK) {
      gtext_css_computed_free(style);
      style = NULL;
      fail(&b, st, NULL, 0, "the highlight's inherited values could not be made");
    }
  }
  gtext_css_block_free(block);
  gtext_css_candidates_free(b.set);
  if (st != GTEXT_CSS_OK || *again) {
    gtext_css_cascaded_free(cascaded);
    for (size_t i = 0; i < b.ndiag; i++) {
      gtext_allocator_free(&context->alloc, b.diag[i].name_copy);
    }
    gtext_allocator_free(&context->alloc, b.diag);
    css_anim_untimed_free(&context->alloc, &untimed);
    return st;
  }
  style->anim_untimed = untimed;
  style->cascaded = cascaded;
  style->diag = b.diag;
  style->ndiag = b.ndiag;
  style->diag_total = b.diag_total;
  *out = style;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_style_compute(const GTEXT_CSS_Compute_Context * context, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Computed_Style * parent_style, const GTEXT_CSS_Style_Entry * entries, size_t count, const GTEXT_CSS_Style_Options * options,
    GTEXT_CSS_Computed_Style ** out, GTEXT_CSS_Compute_Result * result) {
  style_picks picks;
  memset(&picks, 0, sizeof picks);
  GTEXT_CSS_Status st;
  for (unsigned pass = 1;; pass++) {
    bool again = false;
    st = style_compute_pass(context, element, parent_style, entries, count, options, &picks, pass >= MAX_ROLL_PASSES, &again, out, result);
    if (st != GTEXT_CSS_OK || !again) {
      break;
    }
  }
  if (picks.v) {
    GTEXT_Allocator a = context->alloc;
    gtext_allocator_free(&a, picks.v);
  }
  return st;
}

/* ---- reads -------------------------------------------------------------------------------------- */

bool gtext_css_computed_specified(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property, GTEXT_CSS_Cascade_Entry * entry) {
  if (!style || !style->cascaded || property == 0) {
    return false;
  }
  GTEXT_CSS_Cascade_Entry scratch;
  return gtext_css_cascaded_find(style->cascaded, property, NULL, 0, entry ? entry : &scratch);
}

bool gtext_css_computed_specified_custom(const GTEXT_CSS_Computed_Style * style, const char * name, size_t length, GTEXT_CSS_Cascade_Entry * entry) {
  if (!style || !style->cascaded || !name) {
    return false;
  }
  GTEXT_CSS_Cascade_Entry scratch;
  return gtext_css_cascaded_find(style->cascaded, 0, name, length, entry ? entry : &scratch);
}

bool gtext_css_computed_animation_untimed(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property) {
  if (!style || !property) {
    return false;
  }
  const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(style->cc->registry, property);
  for (size_t i = 0; i < style->anim_untimed.nids; i++) {
    if (style->anim_untimed.ids[i] == id) {
      return true;
    }
  }
  return false;
}

bool gtext_css_computed_animation_untimed_custom(const GTEXT_CSS_Computed_Style * style, const char * name, size_t length) {
  if (!style || !name) {
    return false;
  }
  for (size_t i = 0; i < style->anim_untimed.nnames; i++) {
    if (style->anim_untimed.name_lengths[i] == length && memcmp(style->anim_untimed.names[i], name, length) == 0) {
      return true;
    }
  }
  return false;
}

const GTEXT_CSS_Cascaded * gtext_css_computed_cascaded(const GTEXT_CSS_Computed_Style * style) {
  return style ? style->cascaded : NULL;
}

size_t gtext_css_computed_diagnostic_total(const GTEXT_CSS_Computed_Style * style) {
  return style ? style->diag_total : 0;
}

bool gtext_css_computed_diagnostic(const GTEXT_CSS_Computed_Style * style, size_t index, GTEXT_CSS_Style_Diagnostic * out) {
  if (!style || !out || index >= style->ndiag) {
    return false;
  }
  const css_style_diag * d = &style->diag[index];
  out->kind = (GTEXT_CSS_Style_Diagnostic_Kind)d->kind;
  out->source = (GTEXT_CSS_Style_Source)d->source;
  out->entry = d->entry;
  out->name.data = d->name_copy;
  out->name.length = d->name_length;
  out->offset = d->offset;
  out->length = d->length;
  out->line = d->line;
  out->column = d->column;
  return true;
}
