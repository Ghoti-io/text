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
 * `style()` queries (Conditional 5, 6.2): a style feature is a declaration, a property name, or a range, asked of the computed values of the query container.
 *
 * "A <style-feature-plain> evaluates to true if the computed value of the given property on the query container matches the given value (which is also computed with respect
 * to the query container)". The queried value is computed by the ordinary computation: a block of the one declaration is computed as the child of the container (so `em`
 * is the container's font size and `var()` reads the container's custom properties), and its computed value is compared with the container's own by the canonical
 * serialization of the computed value (the dump's equality). A CSS-wide keyword is computed as it would be on the container itself (its parent is the container's
 * parent), and `revert` and `revert-layer` are cascade-dependent, so they make the query false. A shorthand is true when every longhand matches.
 *
 * A style feature without a value is true if the computed value differs from the initial value (a custom property: if it is not the guaranteed-invalid value). A range is
 * evaluated by the steps of the draft: each operand is substituted (a custom property name as `var()`), read as a number, percentage, length, angle, time, frequency or
 * resolution, and compared when the types are alike.
 */

#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_custom.h>
#include <ghoti.io/text/css/css_parse.h>
#include <ghoti.io/text/css/css_shorthand.h>
#include <ghoti.io/text/css/css_value.h>

#include <string.h>

#include "css_container_internal.h"

void css_cq_style_free(struct GTEXT_CSS_Condition_Call_Internal * ci) {
  (void)ci; /* a style query keeps nothing in the call: the compute context is the caller's */
}

void css_cq_style_reset(struct GTEXT_CSS_Condition_Call_Internal * ci) {
  (void)ci;
}

/// The compute context the caller's styles were computed with (the interface's).
static GTEXT_CSS_Status compute_context(css_cq_ctx * cq, const GTEXT_CSS_Compute_Context ** out) {
  if (!cq->c->container.compute_context) {
    cq->call->detail = "container.compute_context";
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  *out = cq->c->container.compute_context;
  return GTEXT_CSS_OK;
}

static bool is_custom(const char * name, size_t len) {
  return len > 2 && name[0] == '-' && name[1] == '-';
}

static bool ieq_n(const char * a, size_t n, const char * lit) {
  if (strlen(lit) != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + 32);
    }
    if (c != lit[i]) {
      return false;
    }
  }
  return true;
}

/// The computed style of the container's inheritance parent (NULL: none, or not computed).
static const GTEXT_CSS_Computed_Style * parent_style(css_cq_ctx * cq) {
  GTEXT_CSS_Element_Handle p = NULL;
  if (!cq->iface || gtext_css_element_get_inheritance_parent(cq->iface, cq->container, &p, NULL) != GTEXT_CSS_OK || !p) {
    return NULL;
  }
  return cq->c->container.computed_style ? cq->c->container.computed_style(cq->c->container.user, p) : NULL;
}

typedef enum { DECL_OK = 0, DECL_UNKNOWN, DECL_FALSE } decl_result;

/// Compute `name: value` as a declaration of the query container: a child of `parent` (the container, or its parent for a CSS-wide keyword). The block is returned for its items.
static GTEXT_CSS_Status compute_decl(css_cq_ctx * cq, const char * name, size_t nlen, const char * value, size_t vlen, const GTEXT_CSS_Computed_Style * parent,
    GTEXT_CSS_Declaration_Block ** block_out, GTEXT_CSS_Computed_Style ** child_out, decl_result * dr) {
  *block_out = NULL;
  *child_out = NULL;
  *dr = DECL_OK;
  const GTEXT_CSS_Compute_Context * ctx = NULL;
  GTEXT_CSS_Status st = compute_context(cq, &ctx);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Declaration_Block * block = NULL;
  st = gtext_css_block_new(cq->c->registry, NULL, &block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_INVALID;
  st = gtext_css_block_add_text(block, name, nlen, value, vlen, false, NULL, &outcome, NULL);
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(block);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      *dr = DECL_UNKNOWN;
      return GTEXT_CSS_OK;
    }
    return st;
  }
  if (outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN) {
    gtext_css_block_free(block);
    *dr = DECL_UNKNOWN;
    return GTEXT_CSS_OK;
  }
  if (outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
    gtext_css_block_free(block);
    *dr = DECL_FALSE;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Computed_Style * child = NULL;
  GTEXT_CSS_Compute_Result cr;
  memset(&cr, 0, sizeof cr);
  st = gtext_css_compute_style(ctx, block, parent, NULL, &child, &cr);
  if (st == GTEXT_CSS_E_INVALID || st == GTEXT_CSS_E_UNSUPPORTED) {
    /* a `revert` (cascade-dependent), or a unit or callback the computation lacks (a `cqw`, an `ex` without a font): the query cannot be answered as true */
    gtext_css_block_free(block);
    *dr = st == GTEXT_CSS_E_INVALID ? DECL_FALSE : DECL_UNKNOWN;
    return GTEXT_CSS_OK;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(block);
    return st;
  }
  *block_out = block;
  *child_out = child;
  return GTEXT_CSS_OK;
}

/// Whether the two styles have the same computed value of a custom property (both none, or the same canonical text).
static GTEXT_CSS_Status custom_equal(css_cq_ctx * cq, const GTEXT_CSS_Computed_Style * a, const GTEXT_CSS_Computed_Style * b, const char * name, size_t nlen, bool * equal) {
  const GTEXT_CSS_Custom_Value *va = NULL, *vb = NULL;
  const bool ha = gtext_css_computed_custom_find(a, name, nlen, &va);
  const bool hb = gtext_css_computed_custom_find(b, name, nlen, &vb);
  *equal = false;
  if (ha != hb) {
    return GTEXT_CSS_OK;
  }
  if (!ha) {
    *equal = true;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &cq->c->alloc;
  char *ta = NULL, *tb = NULL;
  size_t la = 0, lb = 0;
  GTEXT_CSS_Status st = gtext_css_custom_value_text(va, &so, &ta, &la);
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_custom_value_text(vb, &so, &tb, &lb);
  }
  if (st == GTEXT_CSS_OK) {
    *equal = la == lb && (la == 0 || memcmp(ta, tb, la) == 0);
  }
  gtext_css_serialize_buffer_free(&so, ta);
  gtext_css_serialize_buffer_free(&so, tb);
  return st;
}

typedef struct {
  const GTEXT_Allocator * alloc;
  char * data;
  size_t len, cap;
  bool failed;
} text_buf;

static GTEXT_Markup_Status buf_write(void * user, const char * bytes, size_t n) {
  text_buf * b = user;
  if (b->failed) {
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + n + 1) {
      nc *= 2;
    }
    char * nd = gtext_allocator_realloc(b->alloc, b->data, nc);
    if (!nd) {
      b->failed = true;
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = nd;
    b->cap = nc;
  }
  memcpy(b->data + b->len, bytes, n);
  b->len += n;
  b->data[b->len] = 0;
  return GTEXT_MARKUP_OK;
}

/// Whether the two styles have the same computed value of a property (the canonical serialization).
static GTEXT_CSS_Status prop_equal(css_cq_ctx * cq, const GTEXT_CSS_Computed_Style * a, const GTEXT_CSS_Computed_Style * b, GTEXT_CSS_Property_Id p, bool * equal) {
  *equal = false;
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &cq->c->alloc;
  text_buf ba = {&cq->c->alloc, NULL, 0, 0, false}, bb = {&cq->c->alloc, NULL, 0, 0, false};
  GTEXT_Markup_Sink sa = {buf_write, &ba}, sb = {buf_write, &bb};
  GTEXT_CSS_Status st = gtext_css_computed_serialize(a, p, &so, &sa);
  GTEXT_CSS_Status st2 = gtext_css_computed_serialize(b, p, &so, &sb);
  if (ba.failed || bb.failed) {
    st = GTEXT_CSS_E_OOM;
  }
  else if (st == GTEXT_CSS_E_UNSUPPORTED || st2 == GTEXT_CSS_E_UNSUPPORTED) {
    st = GTEXT_CSS_OK; /* a property with no computed value on a side: unequal */
  }
  else if (st == GTEXT_CSS_OK && st2 == GTEXT_CSS_OK) {
    *equal = ba.len == bb.len && (ba.len == 0 || memcmp(ba.data, bb.data, ba.len) == 0);
  }
  else if (st == GTEXT_CSS_OK) {
    st = st2;
  }
  gtext_allocator_free(&cq->c->alloc, ba.data);
  gtext_allocator_free(&cq->c->alloc, bb.data);
  return st;
}

/// Compare the container's computed value(s) of the declared property with those of the child: true if every longhand matches.
static GTEXT_CSS_Status block_equal(css_cq_ctx * cq, const GTEXT_CSS_Declaration_Block * block, const GTEXT_CSS_Computed_Style * child, bool * equal) {
  *equal = true;
  const size_t n = gtext_css_block_count(block);
  for (size_t i = 0; i < n && *equal; i++) {
    GTEXT_CSS_Declaration_Item it;
    if (!gtext_css_block_item(block, i, &it)) {
      continue;
    }
    GTEXT_CSS_Status st;
    if (it.custom_name.data) {
      st = custom_equal(cq, cq->style, child, it.custom_name.data, it.custom_name.length, equal);
    }
    else {
      st = prop_equal(cq, cq->style, child, it.property, equal);
    }
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  return GTEXT_CSS_OK;
}

static bool wide_keyword(const char * v, size_t n, bool * cascade_dependent) {
  *cascade_dependent = ieq_n(v, n, "revert") || ieq_n(v, n, "revert-layer");
  return *cascade_dependent || ieq_n(v, n, "initial") || ieq_n(v, n, "inherit") || ieq_n(v, n, "unset");
}

static GTEXT_CSS_Tristate tri(bool b) {
  return b ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
}

/// `style(name: value)`.
static GTEXT_CSS_Tristate eval_plain(css_cq_ctx * cq, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  bool revert = false;
  const bool wide = wide_keyword(n->value, n->value_length, &revert);
  if (revert) {
    return GTEXT_CSS_TRI_FALSE; /* "Cascade-dependent keywords, such as revert and revert-layer, are invalid as values in a style feature, and cause the container style query to be false." */
  }
  const GTEXT_CSS_Computed_Style * parent = cq->style;
  if (wide) {
    parent = parent_style(cq); /* the remaining CSS-wide keywords are computed with respect to the query container: as if declared on it */
  }
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Computed_Style * child = NULL;
  decl_result dr;
  GTEXT_CSS_Status st = compute_decl(cq, n->text, n->text_length, n->value, n->value_length, parent, &block, &child, &dr);
  if (st != GTEXT_CSS_OK) {
    *status = st;
    *detail = "style()";
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  if (dr == DECL_UNKNOWN) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  if (dr == DECL_FALSE) {
    return GTEXT_CSS_TRI_FALSE;
  }
  bool equal = false;
  st = block_equal(cq, block, child, &equal);
  gtext_css_computed_free(child);
  gtext_css_block_free(block);
  if (st != GTEXT_CSS_OK) {
    *status = st;
    *detail = "style()";
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  return tri(equal);
}

/// `style(name)`: a custom property that is not the guaranteed-invalid value; a property whose computed value is not its initial value.
static GTEXT_CSS_Tristate eval_boolean(css_cq_ctx * cq, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  if (is_custom(n->text, n->text_length)) {
    return tri(gtext_css_computed_custom_find(cq->style, n->text, n->text_length, NULL));
  }
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Computed_Style * child = NULL;
  decl_result dr;
  GTEXT_CSS_Status st = compute_decl(cq, n->text, n->text_length, "initial", 7, NULL, &block, &child, &dr);
  if (st != GTEXT_CSS_OK) {
    *status = st;
    *detail = "style()";
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  if (dr != DECL_OK) {
    return dr == DECL_UNKNOWN ? GTEXT_CSS_TRI_UNKNOWN : GTEXT_CSS_TRI_FALSE;
  }
  bool equal = true;
  st = block_equal(cq, block, child, &equal);
  gtext_css_computed_free(child);
  gtext_css_block_free(block);
  if (st != GTEXT_CSS_OK) {
    *status = st;
    *detail = "style()";
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  return tri(!equal);
}

/* ---- ranges ------------------------------------------------------------------------------------------------------- */

typedef struct {
  double v;
  GTEXT_CSS_Unit_Type type;  ///< the unit's type; NONE for a number
  bool percentage;
  bool number;
} operand;

/// The operand's value read as a number, a percentage or a dimension of an absolute unit; false: it cannot be (the comparison is false).
static GTEXT_CSS_Status read_operand(css_cq_ctx * cq, const char * text, size_t len, operand * op, bool * ok) {
  *ok = false;
  /* a custom property name is `var(--name)`; any other operand is substituted by being computed as the value of a custom property of the child */
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Computed_Style * child = NULL;
  decl_result dr;
  GTEXT_CSS_Status st;
  if (is_custom(text, len)) {
    const GTEXT_CSS_Custom_Value * cv = NULL;
    if (!gtext_css_computed_custom_find(cq->style, text, len, &cv)) {
      return GTEXT_CSS_OK;
    }
    GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
    so.allocator = &cq->c->alloc;
    char * t = NULL;
    size_t tl = 0;
    st = gtext_css_custom_value_text(cv, &so, &t, &tl);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    st = compute_decl(cq, "--css-container-operand", 23, t, tl, cq->style, &block, &child, &dr);
    gtext_css_serialize_buffer_free(&so, t);
  }
  else {
    st = compute_decl(cq, "--css-container-operand", 23, text, len, cq->style, &block, &child, &dr);
  }
  if (st != GTEXT_CSS_OK || dr != DECL_OK) {
    return st;
  }
  const GTEXT_CSS_Custom_Value * cv = NULL;
  if (gtext_css_computed_custom_find(child, "--css-container-operand", 23, &cv)) {
    GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
    so.allocator = &cq->c->alloc;
    char * t = NULL;
    size_t tl = 0;
    st = gtext_css_custom_value_text(cv, &so, &t, &tl);
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Parse_Options po;
      memset(&po, 0, sizeof po);
      po.allocator = &cq->c->alloc;
      GTEXT_CSS_Tree * tree = NULL;
      GTEXT_CSS_Result pr;
      memset(&pr, 0, sizeof pr);
      st = gtext_css_parse_component_values(t, tl, &po, &tree, &pr);
      gtext_css_result_free(&pr);
      if (st == GTEXT_CSS_OK) {
        const GTEXT_CSS_Component_Value * tok = NULL;
        size_t sig = 0;
        for (size_t i = 0; i < tree->value_count; i++) {
          if (!(tree->values[i].kind == GTEXT_CSS_VALUE_TOKEN && tree->values[i].type == GTEXT_CSS_TOKEN_WHITESPACE)) {
            tok = &tree->values[i];
            sig++;
          }
        }
        if (sig == 1 && tok->kind == GTEXT_CSS_VALUE_TOKEN) {
          memset(op, 0, sizeof *op);
          if (tok->type == GTEXT_CSS_TOKEN_NUMBER) {
            op->v = tok->number;
            op->number = true;
            *ok = true;
          }
          else if (tok->type == GTEXT_CSS_TOKEN_PERCENTAGE) {
            op->v = tok->number;
            op->percentage = true;
            *ok = true;
          }
          else if (tok->type == GTEXT_CSS_TOKEN_DIMENSION) {
            const GTEXT_CSS_Unit u = gtext_css_unit_lookup(tok->unit.data, tok->unit.length);
            double canon = 0;
            if (u != GTEXT_CSS_UNIT_NONE && gtext_css_unit_to_canonical(u, tok->number, &canon)) {
              op->v = canon;
              op->type = gtext_css_unit_type(u);
              *ok = true;
            }
          }
        }
              gtext_css_tree_free(tree);
      }
    }
    gtext_css_serialize_buffer_free(&so, t);
  }
  gtext_css_computed_free(child);
  gtext_css_block_free(block);
  return st;
}

static bool compare_op(double a, unsigned op, double b) {
  switch (op) {
  case MQOP_LT: return a < b;
  case MQOP_LE: return a <= b;
  case MQOP_GT: return a > b;
  case MQOP_GE: return a >= b;
  default: return a == b;
  }
}

static GTEXT_CSS_Tristate eval_range(css_cq_ctx * cq, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  const char * txt[3] = {n->text, n->name ? n->name : n->value, n->name ? n->value : NULL};
  const size_t len[3] = {n->text_length, n->name ? n->name_length : n->value_length, n->name ? n->value_length : 0};
  const size_t cnt = n->name ? 3 : 2;
  operand ops[3];
  for (size_t i = 0; i < cnt; i++) {
    bool ok = false;
    const GTEXT_CSS_Status st = read_operand(cq, txt[i], len[i], &ops[i], &ok);
    if (st != GTEXT_CSS_OK) {
      *status = st;
      *detail = "style()";
      return GTEXT_CSS_TRI_UNKNOWN;
    }
    if (!ok) {
      return GTEXT_CSS_TRI_FALSE; /* "If this cannot be done, evaluate to false." */
    }
  }
  /* "If each <style-range-value> from the range have the same type ... a unitless zero must be treated as being a zero-<length> when compared against a <length>. Otherwise evaluate to false." */
  for (size_t i = 1; i < cnt; i++) {
    if (ops[i].number != ops[0].number || ops[i].percentage != ops[0].percentage || ops[i].type != ops[0].type) {
      const bool zero_a = ops[0].number && ops[0].v == 0 && ops[i].type == GTEXT_CSS_UNIT_TYPE_LENGTH;
      const bool zero_b = ops[i].number && ops[i].v == 0 && ops[0].type == GTEXT_CSS_UNIT_TYPE_LENGTH;
      if (!zero_a && !zero_b) {
        return GTEXT_CSS_TRI_FALSE;
      }
    }
  }
  bool r = compare_op(ops[0].v, n->op1, ops[1].v);
  if (cnt == 3) {
    r = r && compare_op(ops[1].v, n->op2, ops[2].v);
  }
  return tri(r);
}

GTEXT_CSS_Tristate css_cq_eval_style(css_cq_ctx * cq, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  (void)in;
  if (!cq->style) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  switch ((mq_form)n->form) {
  case MQFORM_PLAIN: return eval_plain(cq, n, status, detail);
  case MQFORM_BOOLEAN: return eval_boolean(cq, n, status, detail);
  case MQFORM_RANGE: return eval_range(cq, n, status, detail);
  }
  return GTEXT_CSS_TRI_UNKNOWN;
}
