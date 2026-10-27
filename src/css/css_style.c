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

#include "css_compute_internal.h"

#include <ghoti.io/text/css/css_presentation.h>
#include <ghoti.io/text/css/css_style.h>

#include <string.h>

/** The most diagnostics a style keeps; the rest are counted. */
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
  if (b->ndiag >= MAX_KEPT_DIAGNOSTICS) {
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

GTEXT_CSS_Status gtext_css_style_compute(const GTEXT_CSS_Compute_Context * context, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Computed_Style * parent_style, const GTEXT_CSS_Style_Entry * entries, size_t count, const GTEXT_CSS_Style_Options * options,
    GTEXT_CSS_Computed_Style ** out, GTEXT_CSS_Compute_Result * result) {
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
  if (parent_style && parent_style->cc != context) {
    return fail(&b, GTEXT_CSS_E_INVALID, NULL, 0, "the parent style was made for another context");
  }
  if (options->sources != 0 && !element) {
    return fail(&b, GTEXT_CSS_E_INVALID, NULL, 0, "attributes are read, and there is no element");
  }
  const GTEXT_CSS_Element_Interface * iface = context->has_iface ? &context->iface : NULL;
  if (options->sources != 0 && !iface) {
    return fail(&b, GTEXT_CSS_E_UNSUPPORTED, "attribute", 9, "the context has no element interface to read attributes through");
  }
  GTEXT_CSS_Candidate_Options co = options->candidates;
  if (!co.allocator) {
    co.allocator = &context->alloc;
  }
  GTEXT_CSS_Status st = gtext_css_candidates_new(context->registry, options->layers, &co, &b.set);
  if (st != GTEXT_CSS_OK) {
    return fail(&b, st, NULL, 0, NULL);
  }
  if (options->sources & GTEXT_CSS_STYLE_PRESENTATION_ATTRIBUTES) {
    st = add_presentation_attributes(&b, iface, element);
  }
  if (st == GTEXT_CSS_OK && (options->sources & GTEXT_CSS_STYLE_STYLE_ATTRIBUTE)) {
    GTEXT_CSS_String value = {NULL, 0};
    bool found = false;
    st = read_attribute(&b, iface, element, "style", 5, &value, &found);
    if (st == GTEXT_CSS_OK && found) {
      GTEXT_CSS_Cascade_Entry keys;
      memset(&keys, 0, sizeof keys);
      keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
      keys.element_attached = true;
      st = add_list(&b, &keys, value.data, value.length, options->style_attribute_offset, GTEXT_CSS_STYLE_SOURCE_STYLE_ATTRIBUTE, 0);
    }
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < count; i++) {
    GTEXT_CSS_Cascade_Entry keys = entries[i].keys;
    keys.presentational_hint = false;
    keys.element_attached = false;
    st = add_list(&b, &keys, entries[i].text.data ? entries[i].text.data : "", entries[i].text.length, entries[i].base_offset, GTEXT_CSS_STYLE_SOURCE_ENTRY,
        i);
  }
  GTEXT_CSS_Cascaded * cascaded = NULL;
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Computed_Style * style = NULL;
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
  if (st == GTEXT_CSS_OK) {
    const size_t before = b.work;
    GTEXT_CSS_Compute_Result cres;
    st = gtext_css_compute_style(context, block, parent_style, element, &style, &cres);
    if (result) {
      *result = cres;
      result->work += before;
    }
    else {
      (void)before;
    }
  }
  gtext_css_block_free(block);
  gtext_css_candidates_free(b.set);
  if (st != GTEXT_CSS_OK) {
    gtext_css_cascaded_free(cascaded);
    for (size_t i = 0; i < b.ndiag; i++) {
      gtext_allocator_free(&context->alloc, b.diag[i].name_copy);
    }
    gtext_allocator_free(&context->alloc, b.diag);
    return st;
  }
  style->cascaded = cascaded;
  style->diag = b.diag;
  style->ndiag = b.ndiag;
  style->diag_total = b.diag_total;
  *out = style;
  return GTEXT_CSS_OK;
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
