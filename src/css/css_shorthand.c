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
 * Shorthand expansion (css_shorthand.h): a shorthand's typed value is written back as text for each longhand (exactly: numbers as the shortest
 * digits that read back), and each text is parsed by its longhand's own grammar, so that a longhand gets precisely the typed value that
 * writing it would have given. A longhand that is itself a shorthand is expanded in its turn, through a work queue: nothing here recurses.
 */

#include <ghoti.io/math/decimal.h>

#include <stdlib.h>
#include <string.h>

#include "css_grid_text.h"
#include "css_shorthand_internal.h"

static bool kw_is(const GTEXT_CSS_Value * v, const char * word) {
  if (!v || v->kind != GTEXT_CSS_VAL_KEYWORD || !v->text.data) {
    return false;
  }
  const size_t n = strlen(word);
  if (v->text.length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = v->text.data[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != word[i]) {
      return false;
    }
  }
  return true;
}

#define MAX_LONGHANDS_OF_ONE 1024u
#define MAX_RESULT_ITEMS ((size_t)1 << 20)
#define ANY_BUDGET 50000u
#define MAX_LEAVES 128u

/* ---- text ----------------------------------------------------------------------------------------------- */

void tb_init(tbuf * b, const GTEXT_Allocator * alloc) {
  memset(b, 0, sizeof *b);
  b->alloc = *alloc;
}

bool tb_put(tbuf * b, const char * s, size_t n) {
  if (b->oom) {
    return false;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 128;
    while (cap < b->len + n + 1) {
      if (cap > SIZE_MAX / 2) {
        b->oom = true;
        return false;
      }
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!grown) {
      b->oom = true;
      return false;
    }
    b->data = grown;
    b->cap = cap;
  }
  if (n) {
    memcpy(b->data + b->len, s, n);
  }
  b->len += n;
  b->data[b->len] = '\0';
  return true;
}

bool tb_puts(tbuf * b, const char * s) {
  return tb_put(b, s, strlen(s));
}

char * tb_take(tbuf * b, size_t * len) {
  if (b->oom) {
    gtext_allocator_free(&b->alloc, b->data);
    memset(&*b, 0, sizeof *b);
    return NULL;
  }
  if (!b->data) {
    char * z = gtext_allocator_malloc(&b->alloc, 1);
    if (z) {
      z[0] = '\0';
    }
    if (len) {
      *len = 0;
    }
    return z;
  }
  char * d = b->data;
  if (len) {
    *len = b->len;
  }
  b->data = NULL;
  b->len = b->cap = 0;
  return d;
}

void tb_free(tbuf * b) {
  gtext_allocator_free(&b->alloc, b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

size_t css_exact_number(double v, char * out) {
  size_t k = 0;
  if (!(v == v) || v - v != 0.0) { /* NaN and the infinities are not numbers a token can spell */
    out[0] = '0';
    return 1;
  }
  GMATH_Decimal d;
  if (gmath_decimal_shortest(v, &d) != GMATH_OK) {
    out[0] = '0';
    return 1;
  }
  if (d.length == 1 && d.digits[0] == '0') {
    out[0] = '0';
    return 1;
  }
  if (d.negative) {
    out[k++] = '-';
  }
  const int n = (int)d.length;
  const int exp10 = d.exponent + n - 1;
  if (exp10 >= 21 || exp10 <= -7) {
    out[k++] = d.digits[0];
    if (n > 1) {
      out[k++] = '.';
      memcpy(out + k, d.digits + 1, (size_t)n - 1);
      k += (size_t)n - 1;
    }
    out[k++] = 'e';
    {
      int e = exp10;
      char digits[8];
      size_t nd = 0;
      if (e < 0) {
        out[k++] = '-';
        e = -e;
      }
      do {
        digits[nd++] = (char)('0' + e % 10);
        e /= 10;
      } while (e && nd < sizeof digits);
      while (nd) {
        out[k++] = digits[--nd];
      }
    }
    return k;
  }
  if (d.exponent >= 0) {
    memcpy(out + k, d.digits, (size_t)n);
    k += (size_t)n;
    for (int i = 0; i < d.exponent; i++) {
      out[k++] = '0';
    }
    return k;
  }
  const int point = n + d.exponent;
  if (point > 0) {
    memcpy(out + k, d.digits, (size_t)point);
    k += (size_t)point;
    out[k++] = '.';
    memcpy(out + k, d.digits + point, (size_t)(n - point));
    k += (size_t)(n - point);
  }
  else {
    out[k++] = '0';
    out[k++] = '.';
    for (int i = 0; i < -point; i++) {
      out[k++] = '0';
    }
    memcpy(out + k, d.digits, (size_t)n);
    k += (size_t)n;
  }
  return k;
}

static bool exact_leaf(tbuf * b, const GTEXT_CSS_Value * v) {
  char num[96];
  switch (v->kind) {
  case GTEXT_CSS_VAL_NUMBER:
  case GTEXT_CSS_VAL_INTEGER:
    if (!(v->flags & GTEXT_CSS_VFLAG_MATH)) {
      return tb_put(b, num, css_exact_number(v->number, num));
    }
    break;
  case GTEXT_CSS_VAL_PERCENTAGE:
    if (!(v->flags & GTEXT_CSS_VFLAG_MATH)) {
      return tb_put(b, num, css_exact_number(v->number, num)) && tb_put(b, "%", 1);
    }
    break;
  case GTEXT_CSS_VAL_DIMENSION:
    if ((v->flags & GTEXT_CSS_VFLAG_ZERO_LENGTH) && !(v->flags & GTEXT_CSS_VFLAG_MATH)) {
      return tb_put(b, "0", 1);   /* the bare zero the author wrote: the longhand's own grammar reads it as a length or as a number */
    }
    if (!(v->flags & GTEXT_CSS_VFLAG_MATH)) {
      if (!tb_put(b, num, css_exact_number(v->number, num))) {
        return false;
      }
      if (v->unit != GTEXT_CSS_UNIT_NONE) {
        return tb_puts(b, gtext_css_unit_name(v->unit));
      }
      return tb_put(b, v->text.data, v->text.length);
    }
    break;
  case GTEXT_CSS_VAL_ZERO:
    return tb_put(b, "0", 1);
  case GTEXT_CSS_VAL_KEYWORD:
    return tb_put(b, v->text.data, v->text.length);
  case GTEXT_CSS_VAL_RATIO:
    if (!(v->flags & GTEXT_CSS_VFLAG_MATH)) {
      return tb_put(b, num, css_exact_number(v->number, num)) && tb_put(b, " / ", 3) && tb_put(b, num, css_exact_number(v->number2, num));
    }
    break;
  default:
    break;
  }
  char * s = NULL;
  size_t n = 0;
  if (gtext_css_serialize_value_buffer(v, NULL, &s, &n) != GTEXT_CSS_OK) {
    b->oom = true;
    return false;
  }
  const bool ok = tb_put(b, s, n);
  gtext_css_serialize_buffer_free(NULL, s);
  return ok;
}

static bool is_container(const GTEXT_CSS_Value * v) {
  if ((v->flags & GTEXT_CSS_VFLAG_MATH) && v->math) {
    return false;
  }
  return v->kind == GTEXT_CSS_VAL_LIST || v->kind == GTEXT_CSS_VAL_FUNCTION || v->kind == GTEXT_CSS_VAL_BLOCK;
}

bool css_exact_text(tbuf * b, const GTEXT_CSS_Value * root) {
  if (!root) {
    return true;
  }
  if (!is_container(root)) {
    return exact_leaf(b, root);
  }
  typedef struct { const GTEXT_CSS_Value * v; size_t i; } frame;
  size_t cap = 16, sp = 0;
  frame * st = gtext_allocator_malloc(&b->alloc, cap * sizeof *st);
  if (!st) {
    b->oom = true;
    return false;
  }
  st[sp++] = (frame){root, 0};
  bool ok = true;
  while (sp && ok) {
    frame * f = &st[sp - 1];
    const GTEXT_CSS_Value * v = f->v;
    if (f->i == 0) {
      if (v->kind == GTEXT_CSS_VAL_FUNCTION) {
        ok = tb_put(b, v->text.data, v->text.length) && tb_put(b, "(", 1);
      }
      else if (v->kind == GTEXT_CSS_VAL_BLOCK && v->text.length) {
        ok = tb_put(b, v->text.data, 1);
      }
    }
    if (!ok) {
      break;
    }
    if (f->i < v->child_count) {
      if (f->i > 0) {
        const char * sep = " ";
        if (v->kind == GTEXT_CSS_VAL_LIST) {
          sep = v->separator == GTEXT_CSS_SEP_COMMA ? ", " : v->separator == GTEXT_CSS_SEP_SLASH ? " / " : " ";
        }
        ok = tb_puts(b, sep);
      }
      const GTEXT_CSS_Value * kid = &v->children[f->i++];
      if (ok && is_container(kid)) {
        if (sp == cap) {
          if (cap > SIZE_MAX / 2 / sizeof *st) {
            b->oom = true;
            ok = false;
            break;
          }
          frame * grown = gtext_allocator_realloc(&b->alloc, st, cap * 2 * sizeof *st);
          if (!grown) {
            b->oom = true;
            ok = false;
            break;
          }
          st = grown;
          cap *= 2;
        }
        st[sp++] = (frame){kid, 0};
      }
      else if (ok) {
        ok = exact_leaf(b, kid);
      }
      continue;
    }
    if (v->kind == GTEXT_CSS_VAL_FUNCTION) {
      ok = tb_put(b, ")", 1);
    }
    else if (v->kind == GTEXT_CSS_VAL_BLOCK && v->text.length) {
      const char o = v->text.data[0];
      ok = tb_put(b, o == '(' ? ")" : o == '[' ? "]" : "}", 1);
    }
    sp--;
  }
  gtext_allocator_free(&b->alloc, st);
  return ok;
}

/* ---- parsing and writing a property's value ----------------------------------------------------------- */

GTEXT_CSS_Status css_parse_longhand(const GTEXT_CSS_Registry * registry, const GTEXT_Allocator * alloc, GTEXT_CSS_Property_Id property,
    const char * text, size_t length, GTEXT_CSS_Property_Value * pv) {
  GTEXT_CSS_Property_Info info;
  memset(pv, 0, sizeof *pv);
  if (!gtext_css_registry_property_info(registry, property, &info)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = alloc;
  return gtext_css_parse_property_text(registry, info.name, info.name_length, text, length, &po, pv);
}

const char * css_initial_text(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property) {
  const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(registry, property);
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(registry, id, &info)) {
    return NULL;
  }
  /* a property whose initial value the pin leaves out or writes as prose has it in tools/css/computed_rules.tsv */
  const css_tbl_compute * row = css_compute_row(registry, id, NULL);
  if (row && row->initial_len) {
    return gtext_css_tbl_compute_pool + row->initial_off;
  }
  return info.initial;
}

GTEXT_CSS_Status css_item_text(const GTEXT_CSS_Registry * registry, const GTEXT_Allocator * alloc, GTEXT_CSS_Property_Id property,
    GTEXT_CSS_Property_Outcome outcome, GTEXT_CSS_Wide_Keyword wide, const GTEXT_CSS_Value_Set * set, char ** out, size_t * out_len) {
  GTEXT_CSS_Serialize_Options so;
  memset(&so, 0, sizeof so);
  so = gtext_css_serialize_options_default();
  so.allocator = alloc;
  if (outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    const char * k = gtext_css_wide_keyword_name(wide);
    const size_t n = k ? strlen(k) : 0;
    GTEXT_Allocator a = *alloc;
    char * p = gtext_allocator_malloc(&a, n + 1);
    if (!p) {
      return GTEXT_CSS_E_OOM;
    }
    if (n) {
      memcpy(p, k, n);
    }
    p[n] = '\0';
    *out = p;
    *out_len = n;
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  GTEXT_CSS_Status st;
  if (!root) {
    GTEXT_Allocator a = *alloc;
    char * p = gtext_allocator_malloc(&a, 1);
    if (!p) {
      return GTEXT_CSS_E_OOM;
    }
    p[0] = '\0';
    *out = p;
    *out_len = 0;
    return GTEXT_CSS_OK;
  }
  char * raw = NULL;
  size_t raw_len = 0;
  st = gtext_css_serialize_value_buffer(root, &so, &raw, &raw_len);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Property_Info info;
  uint8_t rules = 0;
  if (outcome == GTEXT_CSS_PROP_RESULT_VALID && gtext_css_registry_property_info(registry, property, &info)) {
    rules = css_canon_rules_of(info.name, info.name_length);
  }
  if (!rules) {
    *out = raw;
    *out_len = raw_len;
    return GTEXT_CSS_OK;
  }
  GTEXT_Allocator a = *alloc;
  st = css_canon_apply(rules, &a, raw, raw_len, out, out_len);
  gtext_css_serialize_buffer_free(&so, raw);
  return st;
}

/* ---- kinds ----------------------------------------------------------------------------------------------- */

static const char * const kind_names[GTEXT_CSS_SHORTHAND_KIND_COUNT] = {
  [GTEXT_CSS_SHORTHAND_UNIMPLEMENTED] = "unimplemented",
  [GTEXT_CSS_SHORTHAND_BOX] = "box",
  [GTEXT_CSS_SHORTHAND_PAIR] = "pair",
  [GTEXT_CSS_SHORTHAND_ANY] = "any",
  [GTEXT_CSS_SHORTHAND_LAYERS] = "layers",
  [GTEXT_CSS_SHORTHAND_COPY] = "copy",
  [GTEXT_CSS_SHORTHAND_ALL] = "all",
  [GTEXT_CSS_SHORTHAND_BORDER_RADIUS] = "border-radius",
  [GTEXT_CSS_SHORTHAND_FONT] = "font",
  [GTEXT_CSS_SHORTHAND_FLEX] = "flex",
  [GTEXT_CSS_SHORTHAND_GRID_LINE] = "grid-line",
  [GTEXT_CSS_SHORTHAND_TEXT_ALIGN] = "text-align",
  [GTEXT_CSS_SHORTHAND_FONT_SYNTHESIS] = "font-synthesis",
  [GTEXT_CSS_SHORTHAND_GRID_TEMPLATE] = "grid-template",
  [GTEXT_CSS_SHORTHAND_GRID] = "grid",
  [GTEXT_CSS_SHORTHAND_OFFSET] = "offset",
  [GTEXT_CSS_SHORTHAND_HOOK] = "hook",
};

const char * gtext_css_shorthand_kind_name(GTEXT_CSS_Shorthand_Kind kind) {
  if ((unsigned)kind >= (unsigned)GTEXT_CSS_SHORTHAND_KIND_COUNT) {
    return "unknown";
  }
  return kind_names[kind];
}

GTEXT_CSS_Shorthand_Kind gtext_css_shorthand_kind(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id property) {
  if (!registry) {
    return GTEXT_CSS_SHORTHAND_UNIMPLEMENTED;
  }
  const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(registry, property);
  GTEXT_CSS_Property_Info info;
  if (!id || !gtext_css_registry_property_info(registry, id, &info) || !(info.flags & GTEXT_CSS_PROP_SHORTHAND)) {
    return GTEXT_CSS_SHORTHAND_UNIMPLEMENTED;
  }
  GTEXT_CSS_Shorthand_Hook hook = NULL;
  void * user = NULL;
  if (gtext_css_registry_shorthand_hook(registry, id, &hook, &user) && hook) {
    return GTEXT_CSS_SHORTHAND_HOOK;
  }
  const css_tbl_compute * row = css_compute_row(registry, id, NULL);
  return row ? (GTEXT_CSS_Shorthand_Kind)row->shorthand : GTEXT_CSS_SHORTHAND_UNIMPLEMENTED;
}

/* ---- the list ------------------------------------------------------------------------------------------------ */

struct GTEXT_CSS_Longhand_List {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Longhand * items;
  size_t count, cap;
};

static void list_free_items(GTEXT_CSS_Longhand_List * l) {
  for (size_t i = 0; i < l->count; i++) {
    gtext_css_value_set_free((GTEXT_CSS_Value_Set *)(uintptr_t)l->items[i].set);
  }
}

void gtext_css_longhand_list_free(GTEXT_CSS_Longhand_List * list) {
  if (!list) {
    return;
  }
  GTEXT_Allocator a = list->alloc;
  list_free_items(list);
  gtext_allocator_free(&a, list->items);
  gtext_allocator_free(&a, list);
}

size_t gtext_css_longhand_list_count(const GTEXT_CSS_Longhand_List * list) {
  return list ? list->count : 0;
}

bool gtext_css_longhand_list_at(const GTEXT_CSS_Longhand_List * list, size_t index, GTEXT_CSS_Longhand * out) {
  if (!list || index >= list->count || !out) {
    return false;
  }
  *out = list->items[index];
  return true;
}

static GTEXT_CSS_Status list_append(GTEXT_CSS_Longhand_List * l, GTEXT_CSS_Property_Id property, GTEXT_CSS_Property_Outcome outcome,
    GTEXT_CSS_Wide_Keyword wide, const GTEXT_CSS_Value_Set * set, GTEXT_CSS_Property_Id from, bool reset) {
  if (l->count == MAX_RESULT_ITEMS) {
    return GTEXT_CSS_E_LIMIT_DECLARATIONS;
  }
  /* a later declaration of the same longhand in one expansion replaces the earlier (a longhand listed twice by nested shorthands) */
  size_t at = l->count;
  for (size_t i = 0; i < l->count; i++) {
    if (l->items[i].property == property) {
      at = i;
      break;
    }
  }
  if (at == l->count) {
    if (l->count == l->cap) {
      const size_t cap = l->cap ? l->cap * 2 : 16;
      GTEXT_CSS_Longhand * grown = gtext_allocator_realloc(&l->alloc, l->items, cap * sizeof *grown);
      if (!grown) {
        return GTEXT_CSS_E_OOM;
      }
      l->items = grown;
      l->cap = cap;
    }
    l->count++;
  }
  else {
    gtext_css_value_set_free((GTEXT_CSS_Value_Set *)(uintptr_t)l->items[at].set);
  }
  GTEXT_CSS_Longhand * it = &l->items[at];
  it->property = property;
  it->outcome = outcome;
  it->wide = wide;
  it->set = gtext_css_value_set_retain(set);
  it->from = from;
  it->reset = reset;
  return GTEXT_CSS_OK;
}

/* ---- the expansion ------------------------------------------------------------------------------------------------ */

typedef struct {
  GTEXT_CSS_Property_Value pv;   ///< Owned.
  GTEXT_CSS_Property_Id from;
  bool reset;
} qitem;

typedef struct {
  const GTEXT_CSS_Registry * registry;
  GTEXT_Allocator alloc;
  GTEXT_CSS_Longhand_List * list;
  GTEXT_CSS_Shorthand_Result * res;
  qitem * queue;
  size_t qn, qcap;
  size_t budget;
  size_t steps;   ///< Texts made and components read, besides the assignments `budget` counts: the work of the call (GTEXT_CSS_Shorthand_Result::work).
  GTEXT_CSS_Status status;
  unsigned char * dflt;   ///< The layers the split gave no component of a longhand [longhand * dflt_layers + layer] (box-shadow only): see GTEXT_CSS_VFLAG_DEFAULTED.
  size_t dflt_layers;
  bool oom;   ///< An allocation of the split failed somewhere that returns a string: the expansion is not the value's, whatever else came of it.
} xctx;


static GTEXT_CSS_Status x_fail(xctx * x, GTEXT_CSS_Status status, GTEXT_CSS_Property_Id prop, const char * detail) {
  if (x->oom) {
    /* the value was not split because memory ran out, not because it has no split */
    status = GTEXT_CSS_E_OOM;
    detail = NULL;
  }
  if (x->status == GTEXT_CSS_OK) {
    x->status = status;
    if (x->res) {
      x->res->status = status;
      x->res->message = gtext_css_status_string(status);
      x->res->detail = detail;
      x->res->name[0] = '\0';
      GTEXT_CSS_Property_Info info;
      if (prop && gtext_css_registry_property_info(x->registry, prop, &info)) {
        css_copy_name(x->res->name, sizeof x->res->name, info.name);
      }
    }
  }
  return x->status;
}

static GTEXT_CSS_Status q_push(xctx * x, GTEXT_CSS_Property_Value * pv, GTEXT_CSS_Property_Id from, bool reset) {
  if (x->qn == x->qcap) {
    const size_t cap = x->qcap ? x->qcap * 2 : 8;
    qitem * grown = gtext_allocator_realloc(&x->alloc, x->queue, cap * sizeof *grown);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    x->queue = grown;
    x->qcap = cap;
  }
  x->queue[x->qn].pv = *pv;
  x->queue[x->qn].from = from;
  x->queue[x->qn].reset = reset;
  x->qn++;
  memset(pv, 0, sizeof *pv);
  return GTEXT_CSS_OK;
}

static bool is_shorthand_id(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id) {
  GTEXT_CSS_Property_Info info;
  return gtext_css_registry_property_info(registry, id, &info) && (info.flags & GTEXT_CSS_PROP_SHORTHAND);
}

static bool name_is(const GTEXT_CSS_Registry * registry, GTEXT_CSS_Property_Id id, const char * name) {
  GTEXT_CSS_Property_Info info;
  return gtext_css_registry_property_info(registry, id, &info) && strlen(name) == info.name_length && memcmp(info.name, name, info.name_length) == 0;
}

/// Give a property (a longhand of the shorthand being expanded, or one it resets) a parsed value: a leaf goes to the list, a shorthand to the queue.
static GTEXT_CSS_Status x_deliver(xctx * x, GTEXT_CSS_Property_Id target, GTEXT_CSS_Property_Value * pv, GTEXT_CSS_Property_Id from, bool reset) {
  if (is_shorthand_id(x->registry, target)) {
    pv->property = target;
    GTEXT_CSS_Status s = q_push(x, pv, from, reset);
    return s;
  }
  GTEXT_CSS_Status s = list_append(x->list, target, pv->outcome, pv->wide, pv->set, from, reset);
  gtext_css_property_value_free(pv);
  return s;
}

static GTEXT_CSS_Status x_initial_leaf(xctx * x, GTEXT_CSS_Property_Id target, GTEXT_CSS_Property_Id from, bool reset) {
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  const char * text = css_initial_text(x->registry, target);
  if (text) {
    GTEXT_CSS_Status st = css_parse_longhand(x->registry, &x->alloc, target, text, strlen(text), &pv);
    if (st == GTEXT_CSS_E_OOM) {
      return st;
    }
    if (st == GTEXT_CSS_OK && pv.outcome == GTEXT_CSS_PROP_RESULT_VALID) {
      pv.property = target;
      return x_deliver(x, target, &pv, from, reset);
    }
    gtext_css_property_value_free(&pv);
  }
  /* an initial value that is prose: the keyword */
  memset(&pv, 0, sizeof pv);
  pv.outcome = GTEXT_CSS_PROP_RESULT_WIDE;
  pv.wide = GTEXT_CSS_WIDE_INITIAL;
  pv.property = target;
  return x_deliver(x, target, &pv, from, reset);
}

/// The value of a property that the shorthand gave no component: its initial value, and for a shorthand each longhand's own (all the way down),
/// so that what the block holds is what writing the shorthand would have left.
static GTEXT_CSS_Status x_initial(xctx * x, GTEXT_CSS_Property_Id target, GTEXT_CSS_Property_Id from, bool reset) {
  if (!is_shorthand_id(x->registry, target)) {
    return x_initial_leaf(x, target, from, reset);
  }
  GTEXT_CSS_Property_Id stack[128];
  size_t sp = 0;
  stack[sp++] = target;
  while (sp) {
    const GTEXT_CSS_Property_Id cur = stack[--sp];
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(x->registry, cur, &info)) {
      return GTEXT_CSS_E_INTERNAL;
    }
    for (size_t i = 0; i < info.longhand_count + info.reset_longhand_count; i++) {
      const GTEXT_CSS_Property_Id raw = i < info.longhand_count ? gtext_css_registry_longhand(x->registry, cur, i)
          : gtext_css_registry_reset_longhand(x->registry, cur, i - info.longhand_count);
      const GTEXT_CSS_Property_Id t = gtext_css_registry_resolve_alias(x->registry, raw);
      if (is_shorthand_id(x->registry, t)) {
        if (sp == 128) {
          return x_fail(x, GTEXT_CSS_E_LIMIT_DEPTH, cur, "shorthands nested deeper than the library reads");
        }
        stack[sp++] = t;
        continue;
      }
      GTEXT_CSS_Status st = x_initial_leaf(x, t, from, reset);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
    }
  }
  return GTEXT_CSS_OK;
}

/// The texts of the leaves of a list item (flattened) and a way to split them: used by the split functions.
typedef struct {
  const GTEXT_CSS_Value * v[MAX_LEAVES];
  size_t n;
} leaves_t;

static bool leaves_of(const GTEXT_CSS_Value * root, leaves_t * out) {
  struct { const GTEXT_CSS_Value * node; size_t i; } stack[64];
  size_t sp = 0;
  out->n = 0;
  if (!root) {
    return true;
  }
  if (root->kind != GTEXT_CSS_VAL_LIST || (root->flags & GTEXT_CSS_VFLAG_MATH)) {
    out->v[out->n++] = root;
    return true;
  }
  stack[sp].node = root;
  stack[sp].i = 0;
  sp++;
  while (sp) {
    const GTEXT_CSS_Value * node = stack[sp - 1].node;
    if (stack[sp - 1].i >= node->child_count) {
      sp--;
      continue;
    }
    const GTEXT_CSS_Value * kid = &node->children[stack[sp - 1].i++];
    if (kid->kind == GTEXT_CSS_VAL_LIST && !(kid->flags & GTEXT_CSS_VFLAG_MATH)) {
      if (sp == 64) {
        return false;
      }
      stack[sp].node = kid;
      stack[sp].i = 0;
      sp++;
    }
    else {
      if (out->n == MAX_LEAVES) {
        return false;
      }
      out->v[out->n++] = kid;
    }
  }
  return true;
}

static bool is_slash(const GTEXT_CSS_Value * v) {
  return v->kind == GTEXT_CSS_VAL_TOKEN && v->text.length == 1 && v->text.data[0] == '/';
}

/// The text of a buffer, or NULL; a NULL is remembered, so that a split which goes on without the string cannot pass for one that had none.
static char * x_take(xctx * x, tbuf * b) {
  x->steps += 1 + b->len;
  char * s = tb_take(b, NULL);
  if (!s) {
    x->oom = true;
  }
  return s;
}

static char * text_of(xctx * x, const GTEXT_CSS_Value * v) {
  tbuf b;
  tb_init(&b, &x->alloc);
  css_exact_text(&b, v);
  return x_take(x, &b);
}

static char * dup_text(xctx * x, const char * s) {
  tbuf b;
  tb_init(&b, &x->alloc);
  tb_puts(&b, s);
  return x_take(x, &b);
}

/// The initial value of a longhand as text for a layer: its own initial text, or for a shorthand (`animation-delay`, `background-position`) the
/// initial texts of its longhands one after the other, which is a value of it. A new string; NULL when there is none, or when memory ran out (`*oom`).
static char * initial_layer_text(xctx * x, GTEXT_CSS_Property_Id id, bool * oom) {
  const GTEXT_CSS_Property_Id t = gtext_css_registry_resolve_alias(x->registry, id);
  if (!is_shorthand_id(x->registry, t)) {
    const char * init = css_initial_text(x->registry, t);
    char * copy = init ? dup_text(x, init) : NULL;
    *oom = init && !copy;
    return copy;
  }
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(x->registry, t, &info) || info.longhand_count == 0 || info.longhand_count > 16) {
    return NULL;
  }
  tbuf b;
  tb_init(&b, &x->alloc);
  for (size_t i = 0; i < info.longhand_count; i++) {
    const char * init = css_initial_text(x->registry, gtext_css_registry_longhand(x->registry, t, i));
    if (!init) {
      tb_free(&b);
      return NULL;
    }
    if (i) {
      tb_put(&b, " ", 1);
    }
    tb_puts(&b, init);
  }
  char * made = tb_take(&b, NULL);
  *oom = !made;
  return made;
}


/* ---- BOX, PAIR, COPY ---------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status split_box(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl, char ** txt) {
  const GTEXT_CSS_Value * items = root;
  size_t n = 1;
  if (root && root->kind == GTEXT_CSS_VAL_LIST) {
    items = root->children;
    n = root->child_count;
  }
  if (n < 1 || n > 4 || nl != 4) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a box shorthand with a value of this shape");
  }
  /* top, right, bottom, left: 1: all; 2: top and bottom, right and left; 3: top, right and left, bottom; 4 */
  static const unsigned char pick[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
  for (size_t i = 0; i < 4; i++) {
    txt[i] = text_of(x, &items[pick[n - 1][i]]);
    if (!txt[i]) {
      return GTEXT_CSS_E_OOM;
    }
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status split_pair(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl, char ** txt) {
  const GTEXT_CSS_Value * items = root;
  size_t n = 1;
  if (root && root->kind == GTEXT_CSS_VAL_LIST) {
    items = root->children;
    n = root->child_count;
  }
  if (n < 1 || n > 2 || nl != 2) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a pair shorthand with a value of this shape");
  }
  txt[0] = text_of(x, &items[0]);
  txt[1] = text_of(x, &items[n == 2 ? 1 : 0]);
  if (txt[0] && txt[1] && n == 1 && name_is(x->registry, sh, "place-content") && strstr(txt[0], "baseline")) {
    /* justify-content has no baseline: the fallback alignment stands in (Box Alignment 3, `place-content`) */
    gtext_allocator_free(&x->alloc, txt[1]);
    txt[1] = dup_text(x, "start");
  }
  return txt[0] && txt[1] ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

static GTEXT_CSS_Status split_copy(xctx * x, const GTEXT_CSS_Value * root, size_t nl, char ** txt) {
  for (size_t i = 0; i < nl; i++) {
    txt[i] = text_of(x, root);
    if (!txt[i]) {
      return GTEXT_CSS_E_OOM;
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- border-radius ---------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status split_border_radius(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl, char ** txt) {
  leaves_t l;
  if (!leaves_of(root, &l) || (nl != 4 && nl != 2)) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a border-radius value of this shape");
  }
  size_t slash = l.n;
  for (size_t i = 0; i < l.n; i++) {
    if (is_slash(l.v[i])) {
      slash = i;
      break;
    }
  }
  /* four corners take one to four values (top-left, top-right, bottom-right, bottom-left); the two of a side take one or two */
  static const unsigned char pick4[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
  static const unsigned char pick2[2][2] = {{0, 0}, {0, 1}};
  const size_t nh = slash;
  const size_t nv = slash < l.n ? l.n - slash - 1 : 0;
  const size_t most = nl;
  if (nh < 1 || nh > most || nv > most) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a border-radius value of this shape");
  }
  for (size_t i = 0; i < nl; i++) {
    const size_t hi = nl == 4 ? pick4[nh - 1][i] : pick2[nh - 1][i];
    const GTEXT_CSS_Value * hv = l.v[hi];
    const GTEXT_CSS_Value * vv = nv ? l.v[slash + 1 + (nl == 4 ? pick4[nv - 1][i] : pick2[nv - 1][i])] : hv;
    tbuf b;
    tb_init(&b, &x->alloc);
    css_exact_text(&b, hv);
    if (nv) {
      tbuf c;
      tb_init(&c, &x->alloc);
      css_exact_text(&c, vv);
      if (c.oom) {
        x->oom = true;
        b.oom = true;
      }
      const bool same = c.len == b.len && memcmp(c.data, b.data, b.len) == 0;
      if (!same) {
        tb_put(&b, " ", 1);
        tb_put(&b, c.data, c.len);
      }
      tb_free(&c);
    }
    txt[i] = x_take(x, &b);
    if (!txt[i]) {
      return GTEXT_CSS_E_OOM;
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- flex --------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status split_flex(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl, char ** txt) {
  leaves_t l;
  if (!leaves_of(root, &l) || nl != 3 || l.n < 1 || l.n > 3) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a flex value of this shape");
  }
  char * grow = NULL;
  char * shrink = NULL;
  char * basis = NULL;
  if (l.n == 1 && kw_is(l.v[0], "none")) {
    grow = dup_text(x, "0");
    shrink = dup_text(x, "0");
    basis = dup_text(x, "auto");
  }
  else {
    size_t factors = 0;
    for (size_t i = 0; i < l.n; i++) {
      const GTEXT_CSS_Value * v = l.v[i];
      const bool numeric = ((v->kind == GTEXT_CSS_VAL_NUMBER || v->kind == GTEXT_CSS_VAL_INTEGER || v->kind == GTEXT_CSS_VAL_ZERO ||
          (v->kind == GTEXT_CSS_VAL_DIMENSION && (v->flags & GTEXT_CSS_VFLAG_ZERO_LENGTH))) && !(v->flags & GTEXT_CSS_VFLAG_MATH)) ||
          ((v->flags & GTEXT_CSS_VFLAG_MATH) && v->interp == GTEXT_CSS_INTERP_NUMBER);
      if (numeric && factors < 2 && !(i > 0 && basis == NULL && factors == 1 && false)) {
        /* a unitless number is a flex factor: the first is the grow factor and the second the shrink factor */
        char * t = text_of(x, v);
        if (factors == 0) {
          grow = t;
        }
        else {
          shrink = t;
        }
        factors++;
      }
      else {
        basis = text_of(x, v);
      }
    }
    if (!grow && basis && l.n == 1 && kw_is(l.v[0], "auto")) {
      grow = dup_text(x, "1");
      shrink = dup_text(x, "1");
    }
    else if (!grow) {
      grow = dup_text(x, "1");
    }
    if (!shrink) {
      shrink = dup_text(x, "1");
    }
    if (!basis) {
      basis = dup_text(x, "0%");
    }
  }
  txt[0] = grow;
  txt[1] = shrink;
  txt[2] = basis;
  return grow && shrink && basis ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

/* ---- grid lines, text-align, font-synthesis ------------------------------------------------------------------------------------- */

/// Whether a text is a plain identifier (a `<custom-ident>` that names a grid line): not `auto`, `span`, a number, or several words.
static bool plain_ident(const char * t) {
  if (!t || !t[0] || strcmp(t, "auto") == 0 || strcmp(t, "span") == 0) {
    return false;
  }
  for (const char * q = t; *q; q++) {
    const char c = *q;
    if (c == '\\' && q[1]) {
      /* an escape: hex digits and the one space that ends them, or any other character */
      q++;
      if ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F')) {
        int digits = 1;
        while (digits < 6 && ((q[1] >= '0' && q[1] <= '9') || (q[1] >= 'a' && q[1] <= 'f') || (q[1] >= 'A' && q[1] <= 'F'))) {
          q++;
          digits++;
        }
        if (q[1] == ' ') {
          q++;
        }
      }
      continue;
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || (unsigned char)c >= 0x80 || (q != t && c >= '0' && c <= '9');
    if (!ok) {
      return false;
    }
  }
  return !(t[0] >= '0' && t[0] <= '9') && !(t[0] == '-' && t[1] >= '0' && t[1] <= '9');
}

static GTEXT_CSS_Status split_grid_line(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl, char ** txt) {
  leaves_t l;
  if (!leaves_of(root, &l) || (nl != 2 && nl != 4)) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a grid line value of this shape");
  }
  char * seg[4] = {NULL, NULL, NULL, NULL};
  size_t nseg = 0, from = 0;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  for (size_t i = 0; i <= l.n && status == GTEXT_CSS_OK; i++) {
    if (i == l.n || is_slash(l.v[i])) {
      if (nseg == 4) {
        status = x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "more grid lines than the shorthand has");
        break;
      }
      tbuf b;
      tb_init(&b, &x->alloc);
      for (size_t k = from; k < i; k++) {
        if (k > from) {
          tb_put(&b, " ", 1);
        }
        css_exact_text(&b, l.v[k]);
      }
      seg[nseg++] = x_take(x, &b);
      from = i + 1;
      if (!seg[nseg - 1]) {
        status = GTEXT_CSS_E_OOM;
      }
    }
  }
  if (status == GTEXT_CSS_OK && (nseg < 1 || nseg > nl)) {
    status = x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a grid line value of this shape");
  }
  if (status == GTEXT_CSS_OK) {
    /* an omitted line is the first line's identifier, or auto (Grid 1, 8.4) */
    const char * first = seg[0];
    const char * dflt_first = plain_ident(first) ? first : "auto";
    const char * v[4] = {seg[0], nseg > 1 ? seg[1] : NULL, nseg > 2 ? seg[2] : NULL, nseg > 3 ? seg[3] : NULL};
    if (nl == 2) {
      v[1] = v[1] ? v[1] : dflt_first;
    }
    else {
      v[1] = v[1] ? v[1] : dflt_first;                       /* column-start */
      v[2] = v[2] ? v[2] : dflt_first;                       /* row-end */
      v[3] = v[3] ? v[3] : (plain_ident(v[1]) ? v[1] : "auto");   /* column-end */
    }
    for (size_t i = 0; i < nl; i++) {
      txt[i] = dup_text(x, v[i]);
      if (!txt[i]) {
        status = GTEXT_CSS_E_OOM;
      }
    }
  }
  for (size_t i = 0; i < 4; i++) {
    gtext_allocator_free(&x->alloc, seg[i]);
  }
  return status;
}

/// The slot of a longhand of `grid` in the order rows, columns, areas, auto-rows, auto-columns, auto-flow; -1 for any other.
static int grid_slot(xctx * x, GTEXT_CSS_Property_Id id) {
  static const char * const names[6] = {"grid-template-rows", "grid-template-columns", "grid-template-areas", "grid-auto-rows", "grid-auto-columns",
      "grid-auto-flow"};
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(x->registry, gtext_css_registry_resolve_alias(x->registry, id), &info)) {
    return -1;
  }
  for (int i = 0; i < 6; i++) {
    if (info.name_length == strlen(names[i]) && memcmp(info.name, names[i], info.name_length) == 0) {
      return i;
    }
  }
  return -1;
}

static GTEXT_CSS_Status split_grid(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl, const GTEXT_CSS_Property_Id * ids,
    bool whole, char ** txt) {
  char * text = root ? text_of(x, root) : NULL;
  if (!text) {
    return root ? GTEXT_CSS_E_OOM : x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a grid value of this shape");
  }
  char * out[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
  const css_grid_status st = whole ? css_grid_split(&x->alloc, text, out) : css_grid_template_split(&x->alloc, text, out);
  gtext_allocator_free(&x->alloc, text);
  if (st != CSS_GRID_OK) {
    return st == CSS_GRID_OOM ? GTEXT_CSS_E_OOM : x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a grid value of this shape");
  }
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  for (size_t i = 0; i < nl; i++) {
    const int slot = grid_slot(x, ids[i]);
    if (slot < 0 || (!whole && slot > 2)) {
      status = x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a grid longhand the rule does not know");
      break;
    }
    txt[i] = out[slot];
    out[slot] = NULL;
  }
  for (int i = 0; i < 6; i++) {
    gtext_allocator_free(&x->alloc, out[i]);
  }
  return status;
}

/// `list-style: none` with no other type: the keyword names the type and the image both (Lists 3, `list-style`), so the type is `none` as well.
static void fix_list_style(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, const GTEXT_CSS_Property_Id * ids, size_t nl, char ** txt) {
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(x->registry, sh, &info) || info.name_length != 10 || memcmp(info.name, "list-style", 10) != 0) {
    return;
  }
  int image = -1, type = -1;
  for (size_t i = 0; i < nl; i++) {
    GTEXT_CSS_Property_Info li;
    if (!gtext_css_registry_property_info(x->registry, gtext_css_registry_resolve_alias(x->registry, ids[i]), &li)) {
      return;
    }
    if (li.name_length == 16 && memcmp(li.name, "list-style-image", 16) == 0) {
      image = (int)i;
    }
    else if (li.name_length == 15 && memcmp(li.name, "list-style-type", 15) == 0) {
      type = (int)i;
    }
  }
  leaves_t l;
  bool one_none = false;
  if (root && leaves_of(root, &l)) {
    size_t nones = 0;
    bool typed = false;
    for (size_t i = 0; i < l.n; i++) {
      if (kw_is(l.v[i], "none")) {
        nones++;
      }
      else if (l.v[i]->kind == GTEXT_CSS_VAL_KEYWORD && !kw_is(l.v[i], "inside") && !kw_is(l.v[i], "outside")) {
        typed = true;
      }
      else if (l.v[i]->kind == GTEXT_CSS_VAL_STRING || l.v[i]->kind == GTEXT_CSS_VAL_CUSTOM_IDENT ||
          (l.v[i]->kind == GTEXT_CSS_VAL_FUNCTION && l.v[i]->text.length == 7 && memcmp(l.v[i]->text.data, "symbols", 7) == 0)) {
        typed = true;
      }
    }
    one_none = nones == 1 && !typed;
  }
  if (image >= 0 && type >= 0 && one_none) {
    char * none = dup_text(x, "none");
    if (none) {
      gtext_allocator_free(&x->alloc, txt[type]);
      txt[type] = none;
    }
  }
}

/// The longhand of `ids` named `name` (its index), or nl.
static size_t index_of(xctx * x, const GTEXT_CSS_Property_Id * ids, size_t nl, const char * name) {
  for (size_t i = 0; i < nl; i++) {
    if (name_is(x->registry, gtext_css_registry_resolve_alias(x->registry, ids[i]), name)) {
      return i;
    }
  }
  return nl;
}

/// Shorthands that are the generic assignment plus keywords that stand for values of several longhands: `white-space: pre` and the others of
/// its four legacy keywords (Text 4, `white-space`), `text-spacing: none` and `auto`. True when the value was one of them (and `txt` is filled).
static bool split_keyword_shorthand(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, const GTEXT_CSS_Property_Id * ids, size_t nl,
    char ** txt, GTEXT_CSS_Status * status) {
  if (root && name_is(x->registry, sh, "position-try")) {
    /* [ <'position-try-order'>? <'position-try-fallbacks'> ]: the fallbacks are a comma list, so the order is the first word when it is one of its keywords */
    char * text = text_of(x, root);
    if (!text) {
      *status = GTEXT_CSS_E_OOM;
      return true;
    }
    static const char * const sizes[] = {"normal", "most-width", "most-height", "most-block-size", "most-inline-size"};
    const size_t io = index_of(x, ids, nl, "position-try-order"), ifb = index_of(x, ids, nl, "position-try-fallbacks");
    const char * rest = text;
    const char * order = NULL;
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes && io != nl && ifb != nl; k++) {
      const size_t n = strlen(sizes[k]);
      if (strncmp(text, sizes[k], n) == 0 && (text[n] == ' ' || text[n] == '\0')) {
        order = sizes[k];
        rest = text + n + (text[n] == ' ' ? 1 : 0);
      }
    }
    if (io == nl || ifb == nl) {
      gtext_allocator_free(&x->alloc, text);
      return false;
    }
    txt[io] = dup_text(x, order ? order : "normal");
    txt[ifb] = dup_text(x, rest[0] ? rest : "none");
    gtext_allocator_free(&x->alloc, text);
    *status = txt[io] && txt[ifb] ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    return true;
  }
  leaves_t l;
  if (root && name_is(x->registry, sh, "-webkit-line-clamp") && leaves_of(root, &l) && l.n == 1) {
    /* none, or a line count: the count with the ellipsis and the legacy continuation (Overflow 4, `-webkit-line-clamp`) */
    const size_t im = index_of(x, ids, nl, "max-lines"), ib = index_of(x, ids, nl, "block-ellipsis"), ic = index_of(x, ids, nl, "continue");
    if (im == nl || ib == nl || ic == nl) {
      return false;
    }
    if (kw_is(l.v[0], "none")) {
      txt[im] = dup_text(x, "auto");
      txt[ib] = dup_text(x, "no-ellipsis");
      txt[ic] = dup_text(x, "normal");
    }
    else {
      txt[im] = text_of(x, l.v[0]);
      txt[ib] = dup_text(x, "ellipsis");
      txt[ic] = dup_text(x, "-webkit-legacy");
    }
    *status = txt[im] && txt[ib] && txt[ic] ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    return true;
  }
  if (root && (name_is(x->registry, sh, "column-rule-inset") || name_is(x->registry, sh, "row-rule-inset")) && leaves_of(root, &l)) {
    /* `<'*-rule-inset-cap'> [ / <'*-rule-inset-junction'> ]?`: each is a start and an end, the end being the start when it is left out, and the
       junction being the cap when it is left out (Gaps 1) */
    size_t slash = l.n;
    for (size_t i = 0; i < l.n; i++) {
      if (is_slash(l.v[i])) {
        slash = i;
        break;
      }
    }
    const size_t ncap = slash, njun = slash < l.n ? l.n - slash - 1 : 0;
    if (ncap < 1 || ncap > 2 || (slash < l.n && (njun < 1 || njun > 2))) {
      return false;
    }
    static const char * const suffix[4] = {"-cap-start", "-cap-end", "-junction-start", "-junction-end"};
    for (size_t k = 0; k < 4; k++) {
      char name[40];
      const char * prefix = name_is(x->registry, sh, "column-rule-inset") ? "column-rule-inset" : "row-rule-inset";
      const size_t pl = strlen(prefix), sl = strlen(suffix[k]);
      memcpy(name, prefix, pl);
      memcpy(name + pl, suffix[k], sl + 1);
      const size_t at = index_of(x, ids, nl, name);
      if (at == nl) {
        return false;
      }
      const size_t from = k < 2 ? 0 : slash + 1, n = k < 2 ? ncap : njun;
      const size_t pick = (k & 1) && n == 2 ? 1 : 0;
      if (k >= 2 && slash == l.n) {
        txt[at] = text_of(x, l.v[(k & 1) && ncap == 2 ? 1 : 0]);   /* the cap's own */
      }
      else {
        txt[at] = text_of(x, l.v[from + pick]);
      }
      if (!txt[at]) {
        *status = GTEXT_CSS_E_OOM;
        return true;
      }
    }
    *status = GTEXT_CSS_OK;
    return true;
  }
  if (!root || !leaves_of(root, &l) || l.n != 1 || l.v[0]->kind != GTEXT_CSS_VAL_KEYWORD) {
    return false;
  }
  if (name_is(x->registry, sh, "text-decoration-skip")) {
    /* `none` skips nothing; `auto` is what a user agent does by default (Text Decoration 4, `text-decoration-skip`) */
    const bool none = kw_is(l.v[0], "none");
    if (!none && !kw_is(l.v[0], "auto")) {
      return false;
    }
    static const char * const names[4] = {"text-decoration-skip-self", "text-decoration-skip-box", "text-decoration-skip-spaces", "text-decoration-skip-ink"};
    const char * const values[2][4] = {{"no-skip", "none", "none", "none"}, {"auto", "none", "start end", "auto"}};
    for (size_t k = 0; k < 4; k++) {
      const size_t at = index_of(x, ids, nl, names[k]);
      if (at == nl) {
        return false;
      }
      txt[at] = dup_text(x, values[none ? 0 : 1][k]);
      if (!txt[at]) {
        *status = GTEXT_CSS_E_OOM;
        return true;
      }
    }
    *status = GTEXT_CSS_OK;
    return true;
  }
  static const struct { const char * sh; const char * kw; const char * a; const char * av; const char * b; const char * bv; } table[] = {
    {"white-space", "normal", "white-space-collapse", "collapse", "text-wrap-mode", "wrap"},
    {"white-space", "pre", "white-space-collapse", "preserve", "text-wrap-mode", "nowrap"},
    {"white-space", "pre-wrap", "white-space-collapse", "preserve", "text-wrap-mode", "wrap"},
    {"white-space", "pre-line", "white-space-collapse", "preserve-breaks", "text-wrap-mode", "wrap"},
    {"text-spacing", "none", "text-spacing-trim", "space-all", "text-autospace", "no-autospace"},
    {"text-spacing", "auto", "text-spacing-trim", "auto", "text-autospace", "auto"},
  };
  for (size_t k = 0; k < sizeof table / sizeof *table; k++) {
    if (!name_is(x->registry, sh, table[k].sh) || !kw_is(l.v[0], table[k].kw)) {
      continue;
    }
    const size_t ia = index_of(x, ids, nl, table[k].a), ib = index_of(x, ids, nl, table[k].b);
    if (ia == nl || ib == nl) {
      return false;
    }
    txt[ia] = dup_text(x, table[k].av);
    txt[ib] = dup_text(x, table[k].bv);
    *status = txt[ia] && txt[ib] ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    return true;
  }
  return false;
}

/* ---- offset --------------------------------------------------------------------------------------------------------------------- */

/// Whether a leaf starts or continues the path of an `offset`: `none`, a function (`path()`, `ray()`, a shape), a url or a coord-box keyword.
static bool offset_path_leaf(const GTEXT_CSS_Value * v) {
  if ((v->flags & GTEXT_CSS_VFLAG_MATH) && v->math) {
    return false;
  }
  if (v->kind == GTEXT_CSS_VAL_FUNCTION || v->kind == GTEXT_CSS_VAL_URL) {
    return true;
  }
  return kw_is(v, "none") || kw_is(v, "content-box") || kw_is(v, "padding-box") || kw_is(v, "border-box") || kw_is(v, "margin-box") ||
      kw_is(v, "fill-box") || kw_is(v, "stroke-box") || kw_is(v, "view-box");
}

static char * join_leaves(xctx * x, const leaves_t * l, size_t from, size_t to) {
  tbuf b;
  tb_init(&b, &x->alloc);
  for (size_t i = from; i < to; i++) {
    if (i > from) {
      tb_put(&b, " ", 1);
    }
    css_exact_text(&b, l->v[i]);
  }
  return x_take(x, &b);
}

/// `offset: [ <'offset-position'>? [ <'offset-path'> [ <'offset-distance'> || <'offset-rotate'> ]? ]? ]! [ / <'offset-anchor'> ]?` (Motion Path 1):
/// the position comes before the path, the distance and the rotation after it, so a length before a path is the position and after it the distance.
static GTEXT_CSS_Status split_offset(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl, const GTEXT_CSS_Property_Id * ids,
    char ** txt) {
  leaves_t l;
  if (!root || !leaves_of(root, &l)) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "an offset value of this shape");
  }
  const size_t ipos = index_of(x, ids, nl, "offset-position"), ipath = index_of(x, ids, nl, "offset-path"), idist = index_of(x, ids, nl, "offset-distance"),
      irot = index_of(x, ids, nl, "offset-rotate"), ianc = index_of(x, ids, nl, "offset-anchor");
  if (ipos == nl || ipath == nl || idist == nl || irot == nl || ianc == nl) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "an offset with other longhands than the draft's");
  }
  size_t slash = l.n;
  for (size_t i = 0; i < l.n; i++) {
    if (is_slash(l.v[i])) {
      slash = i;
      break;
    }
  }
  size_t p = 0;
  while (p < slash && !offset_path_leaf(l.v[p])) {
    p++;
  }
  size_t pe = p;
  while (pe < slash && pe < p + 2 && offset_path_leaf(l.v[pe])) {
    pe++;
  }
  if (p < slash && p > 0 && p == slash) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "an offset value of this shape");
  }
  /* no path: everything before the slash is the position */
  const size_t pos_end = p;
  char * pos = pos_end > 0 ? join_leaves(x, &l, 0, pos_end) : NULL;
  char * path = pe > p ? join_leaves(x, &l, p, pe) : NULL;
  tbuf dist, rot;
  tb_init(&dist, &x->alloc);
  tb_init(&rot, &x->alloc);
  for (size_t i = pe; i < slash; i++) {
    const GTEXT_CSS_Value * v = l.v[i];
    const bool angle = (v->kind == GTEXT_CSS_VAL_DIMENSION && !(v->flags & GTEXT_CSS_VFLAG_MATH) && gtext_css_unit_type(v->unit) == GTEXT_CSS_UNIT_TYPE_ANGLE) ||
        kw_is(v, "auto") || kw_is(v, "reverse") || ((v->flags & GTEXT_CSS_VFLAG_MATH) && v->interp == GTEXT_CSS_INTERP_ANGLE);
    tbuf * to = angle ? &rot : &dist;
    if (to->len) {
      tb_put(to, " ", 1);
    }
    css_exact_text(to, v);
  }
  char * anchor = slash < l.n ? join_leaves(x, &l, slash + 1, l.n) : NULL;
  txt[ipos] = pos;
  txt[ipath] = path;
  txt[idist] = dist.len ? x_take(x, &dist) : NULL;
  txt[irot] = rot.len ? x_take(x, &rot) : NULL;
  if (dist.oom || rot.oom) {
    x->oom = true;
  }
  txt[ianc] = anchor;
  tb_free(&dist);
  tb_free(&rot);
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status split_text_align(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl, char ** txt) {
  if (nl != 2 || !root) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a text-align value of this shape");
  }
  char * t = text_of(x, root);
  if (!t) {
    return GTEXT_CSS_E_OOM;
  }
  if (strcmp(t, "justify-all") == 0) {
    gtext_allocator_free(&x->alloc, t);
    txt[0] = dup_text(x, "justify");
    txt[1] = dup_text(x, "justify");
  }
  else {
    txt[0] = t;
    txt[1] = dup_text(x, "auto");
  }
  return txt[0] && txt[1] ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

static GTEXT_CSS_Status split_font_synthesis(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl,
    const GTEXT_CSS_Property_Id * ids, char ** txt) {
  leaves_t l;
  if (!leaves_of(root, &l)) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a font-synthesis value of this shape");
  }
  static const char * const kinds[] = {"weight", "style", "small-caps", "position"};
  for (size_t i = 0; i < nl; i++) {
    bool listed = false;
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(x->registry, gtext_css_registry_resolve_alias(x->registry, ids[i]), &info)) {
      return GTEXT_CSS_E_INTERNAL;
    }
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++) {
      const size_t kl = strlen(kinds[k]);
      if (info.name_length > kl + 1 && memcmp(info.name + info.name_length - kl, kinds[k], kl) == 0 && info.name[info.name_length - kl - 1] == '-') {
        for (size_t j = 0; j < l.n; j++) {
          listed = listed || kw_is(l.v[j], kinds[k]);
        }
      }
    }
    txt[i] = dup_text(x, listed ? "auto" : "none");
    if (!txt[i]) {
      return GTEXT_CSS_E_OOM;
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- font --------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status split_font(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, size_t nl,
    const GTEXT_CSS_Property_Id * ids, char ** txt) {
  if (!root || root->kind != GTEXT_CSS_VAL_LIST || root->child_count < 2) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a system font needs the environment's font table");
  }
  size_t idx = 0;
  char *style = NULL, *variant = NULL, *weight = NULL, *width = NULL, *size = NULL, *lh = NULL, *family = NULL;
  const GTEXT_CSS_Value * c = root->children;
  const size_t n = root->child_count;
  if (c[0].kind == GTEXT_CSS_VAL_LIST && c[0].child_count && c[0].children[0].kind != GTEXT_CSS_VAL_TOKEN) {
    leaves_t l;
    if (!leaves_of(&c[0], &l)) {
      return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a font value of this shape");
    }
    for (size_t i = 0; i < l.n; i++) {
      const GTEXT_CSS_Value * v = l.v[i];
      if (kw_is(v, "normal")) {
        continue;
      }
      if (kw_is(v, "italic")) {
        style = dup_text(x, "italic");
      }
      else if (kw_is(v, "oblique")) {
        tbuf b;
        tb_init(&b, &x->alloc);
        tb_puts(&b, "oblique");
        if (i + 1 < l.n && l.v[i + 1]->kind == GTEXT_CSS_VAL_DIMENSION) {
          tb_puts(&b, " ");
          css_exact_text(&b, l.v[++i]);
        }
        style = x_take(x, &b);
      }
      else if (kw_is(v, "small-caps")) {
        variant = dup_text(x, "small-caps");
      }
      else if (kw_is(v, "bold") || kw_is(v, "bolder") || kw_is(v, "lighter") || v->kind == GTEXT_CSS_VAL_NUMBER || v->kind == GTEXT_CSS_VAL_INTEGER) {
        weight = text_of(x, v);
      }
      else if (v->kind == GTEXT_CSS_VAL_KEYWORD) {
        width = text_of(x, v);
      }
    }
    idx = 1;
  }
  if (idx >= n) {
    char * const pieces[7] = {style, variant, weight, width, size, lh, family};
    for (size_t k = 0; k < 7; k++) {
      gtext_allocator_free(&x->alloc, pieces[k]);
    }
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a font value without a size");
  }
  size = text_of(x, &c[idx++]);
  if (idx < n && c[idx].kind == GTEXT_CSS_VAL_LIST && c[idx].child_count >= 2 && is_slash(&c[idx].children[0])) {
    lh = text_of(x, &c[idx].children[1]);
    idx++;
  }
  if (idx < n) {
    family = text_of(x, &c[idx]);
  }
  if (!size || !family) {
    char * const pieces[7] = {style, variant, weight, width, size, lh, family};
    for (size_t k = 0; k < 7; k++) {
      gtext_allocator_free(&x->alloc, pieces[k]);
    }
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a font value without a family");
  }
  char * const values[7] = {style, variant, weight, width, size, lh, family};
  static const char * const names[7] = {"font-style", "font-variant", "font-weight", "font-width", "font-size", "line-height", "font-family"};
  static const char * const defaults[7] = {"normal", "normal", "normal", "normal", NULL, "normal", NULL};
  for (size_t k = 0; k < 7; k++) {
    for (size_t i = 0; i < nl; i++) {
      if (name_is(x->registry, gtext_css_registry_resolve_alias(x->registry, ids[i]), names[k])) {
        txt[i] = values[k] ? values[k] : (defaults[k] ? dup_text(x, defaults[k]) : NULL);
      }
    }
  }
  /* the pieces that were not placed in a slot are released with the rest by the caller; those that were are owned by txt */
  for (size_t k = 0; k < 7; k++) {
    bool placed = false;
    for (size_t i = 0; i < nl; i++) {
      placed = placed || (txt[i] && txt[i] == values[k]);
    }
    if (!placed) {
      gtext_allocator_free(&x->alloc, values[k]);
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- ANY and LAYERS ------------------------------------------------------------------------------------------------------------- */

typedef struct {
  xctx * x;
  const GTEXT_CSS_Property_Id * ids;   ///< The longhands (direct), as the shorthand lists them.
  size_t nh;
  const leaves_t * l;
  char ** texts;                       ///< Per leaf: its text, lazily made.
  signed char * memo;                  ///< [nh][n+1][n+1]: 0 unknown, 1 accepted, 2 refused.
  size_t n;
  size_t calls;
  bool final_only_mask_active;
  unsigned long long final_only;       ///< Longhands only the last layer may have.
} anyctx;

static const char * leaf_text(anyctx * a, size_t i) {
  if (!a->texts[i]) {
    a->texts[i] = text_of(a->x, a->l->v[i]);
  }
  return a->texts[i];
}

/// 0: the longhand refuses the run; 1: it takes it as a custom identifier (a counter style named `inside`); 2: it takes it as a keyword or anything
/// more specific. A keyword goes to the longhand that lists it before a longhand that merely accepts any identifier.
static int accepts(anyctx * a, size_t h, size_t from, size_t to) {
  signed char * m = &a->memo[(h * (a->n + 1) + from) * (a->n + 1) + to];
  if (*m) {
    return *m - 1;
  }
  tbuf b;
  tb_init(&b, &a->x->alloc);
  for (size_t i = from; i < to; i++) {
    if (i > from) {
      tb_put(&b, " ", 1);
    }
    const char * t = leaf_text(a, i);
    if (!t) {
      b.oom = true;
      break;
    }
    tb_puts(&b, t);
  }
  if (b.oom) {
    tb_free(&b);
    a->x->status = GTEXT_CSS_E_OOM;
    return false;
  }
  a->calls++;
  GTEXT_CSS_Property_Value pv;
  const GTEXT_CSS_Status st = css_parse_longhand(a->x->registry, &a->x->alloc, gtext_css_registry_resolve_alias(a->x->registry, a->ids[h]), b.data,
      b.len, &pv);
  tb_free(&b);
  const bool ok = st == GTEXT_CSS_OK && pv.outcome == GTEXT_CSS_PROP_RESULT_VALID;
  if (st == GTEXT_CSS_E_OOM) {
    a->x->status = st;
  }
  int rank = ok ? 2 : 0;
  if (ok) {
    const GTEXT_CSS_Value * r = gtext_css_value_set_root(pv.set);
    while (r && r->kind == GTEXT_CSS_VAL_LIST && r->child_count) {
      r = &r->children[0];
    }
    if (r && (r->kind == GTEXT_CSS_VAL_CUSTOM_IDENT || r->kind == GTEXT_CSS_VAL_DASHED_IDENT)) {
      rank = 1;
    }
  }
  gtext_css_property_value_free(&pv);
  *m = (signed char)(rank + 1);
  return rank;
}

/// Assign the leaves of one layer to the longhands: segments split at `/` go to longhands in the order the pin lists them; a longhand takes a run of
/// leaves its grammar accepts, longest run first, and no longhand takes two runs. `assigned[h]` receives the run [from, to) or from == to for none.
static GTEXT_CSS_Status assign_layer(anyctx * a, GTEXT_CSS_Property_Id sh, bool last_layer, size_t (*assigned)[2]) {
  xctx * x = a->x;
  const leaves_t * l = a->l;
  for (size_t h = 0; h < a->nh; h++) {
    assigned[h][0] = assigned[h][1] = 0;
  }
  size_t seg_start = 0, floor = 0, seg_index = 0;
  unsigned long long used = 0;
  size_t i = 0;
  /* container: the part before the slash is the name alone and the part after it the type alone (the keywords of one are idents of the other) */
  const bool container = name_is(x->registry, sh, "container");
  while (i <= l->n) {
    size_t seg_end = i;
    while (seg_end < l->n && !is_slash(l->v[seg_end])) {
      seg_end++;
    }
    /* [seg_start, seg_end) is a segment */
    seg_start = i;
    struct { size_t pos, cursor, h, len; } lv[MAX_LEAVES + 1];
    size_t level = 0;
    lv[0].pos = seg_start;
    lv[0].cursor = 0;
    unsigned long long used_before = used;
    bool done = seg_start == seg_end;
    size_t max_h = floor;
    while (!done) {
      if (lv[level].pos == seg_end) {
        break;
      }
      const size_t rem = seg_end - lv[level].pos;
      bool found = false;
      size_t c = lv[level].cursor;
      /* two sweeps: first the longhands that take the run as a keyword or something more specific, then those that take any identifier */
      for (; c < 2 * a->nh * rem; c++) {
        const size_t sweep = c / (a->nh * rem);
        const size_t idx = c % (a->nh * rem);
        const size_t h = idx / rem;
        const size_t len = rem - (idx % rem);
        if ((used >> h) & 1ull || h < floor) {
          continue;
        }
        if (!last_layer && ((a->final_only >> h) & 1ull)) {
          continue;
        }
        if (container && !name_is(x->registry, gtext_css_registry_resolve_alias(x->registry, a->ids[h]), seg_index == 0 ? "container-name" : "container-type")) {
          continue;
        }
        if (++x->budget > ANY_BUDGET) {
          return x_fail(x, GTEXT_CSS_E_LIMIT_WORK, sh, "the split of a shorthand tried too many ways to assign its components");
        }
        const int rank = accepts(a, h, lv[level].pos, lv[level].pos + len);
        if ((sweep == 0 && rank == 2) || (sweep == 1 && rank == 1)) {
          lv[level].h = h;
          lv[level].len = len;
          found = true;
          break;
        }
        if (x->status != GTEXT_CSS_OK) {
          return x->status;
        }
      }
      if (x->status != GTEXT_CSS_OK) {
        return x->status;
      }
      if (found) {
        used |= 1ull << lv[level].h;
        lv[level].cursor = c + 1;
        lv[level + 1].pos = lv[level].pos + lv[level].len;
        lv[level + 1].cursor = 0;
        level++;
      }
      else {
        if (level == 0) {
          return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "the generic split found no assignment of the components of the value to the longhands");
        }
        level--;
        used &= ~(1ull << lv[level].h);
      }
    }
    for (size_t k = 0; k < level; k++) {
      assigned[lv[k].h][0] = lv[k].pos;
      assigned[lv[k].h][1] = lv[k].pos + lv[k].len;
      if (lv[k].h + 1 > max_h) {
        max_h = lv[k].h + 1;
      }
    }
    (void)used_before;
    if (seg_start == seg_end && seg_index > 0 && max_h == floor) {
      max_h = floor + 1;   /* an empty part still stands for a longhand: border-image's `1 / / 1px` has no width and an outset */
    }
    floor = max_h;
    seg_index++;
    i = seg_end + 1;
    if (seg_end >= l->n) {
      break;
    }
  }
  return GTEXT_CSS_OK;
}

/// The leaves of a layered shorthand's value in order, with where each layer starts. The grammar of a shorthand that names a longhand which is
/// itself a list (`animation-range`: `[ <'animation-range-start'> <'animation-range-end'>? ]#`) nests the commas inside the first component, so
/// the layers are read from the commas wherever they are: every child of a comma list but the first begins a layer.
typedef struct {
  const GTEXT_CSS_Value ** v;
  size_t n, cap;
  size_t * bound;
  size_t nb, capb;
} flat_layers;

static void free_flat(xctx * x, flat_layers * f) {
  gtext_allocator_free(&x->alloc, f->v);
  gtext_allocator_free(&x->alloc, f->bound);
  memset(f, 0, sizeof *f);
}

static bool flat_bound(xctx * x, flat_layers * f, size_t at) {
  if (f->nb == f->capb) {
    const size_t cap = f->capb ? f->capb * 2 : 8;
    size_t * nb = gtext_allocator_realloc(&x->alloc, f->bound, cap * sizeof *nb);
    if (!nb) {
      return false;
    }
    f->bound = nb;
    f->capb = cap;
  }
  f->bound[f->nb++] = at;
  return true;
}

static GTEXT_CSS_Status flatten_layers(xctx * x, const GTEXT_CSS_Value * root, flat_layers * f) {
  struct { const GTEXT_CSS_Value * node; size_t i; } stack[64];
  size_t sp = 0;
  if (!flat_bound(x, f, 0)) {
    return GTEXT_CSS_E_OOM;
  }
  if (!root) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Value * start = root;
  if (start->kind != GTEXT_CSS_VAL_LIST || (start->flags & GTEXT_CSS_VFLAG_MATH)) {
    f->v = gtext_allocator_malloc(&x->alloc, sizeof *f->v);
    if (!f->v) {
      return GTEXT_CSS_E_OOM;
    }
    f->v[0] = start;
    f->n = f->cap = 1;
    return GTEXT_CSS_OK;
  }
  stack[sp].node = start;
  stack[sp].i = 0;
  sp++;
  while (sp) {
    const GTEXT_CSS_Value * node = stack[sp - 1].node;
    if (stack[sp - 1].i >= node->child_count) {
      sp--;
      continue;
    }
    const size_t at = stack[sp - 1].i++;
    const GTEXT_CSS_Value * kid = &node->children[at];
    x->steps++;
    if (at > 0 && node->separator == GTEXT_CSS_SEP_COMMA) {
      if (f->nb >= 4096) {
        return GTEXT_CSS_E_LIMIT_LIST;
      }
      if (!flat_bound(x, f, f->n)) {
        return GTEXT_CSS_E_OOM;
      }
    }
    if (kid->kind == GTEXT_CSS_VAL_TOKEN && kid->text.length == 1 && kid->text.data[0] == ',') {
      /* the comma a grammar like `<bg-layer>#? , <final-bg-layer>` writes between its parts */
      if (f->nb >= 4096) {
        return GTEXT_CSS_E_LIMIT_LIST;
      }
      if (!flat_bound(x, f, f->n)) {
        return GTEXT_CSS_E_OOM;
      }
    }
    else if (kid->kind == GTEXT_CSS_VAL_LIST && !(kid->flags & GTEXT_CSS_VFLAG_MATH)) {
      if (sp == 64) {
        return GTEXT_CSS_E_LIMIT_LIST;
      }
      stack[sp].node = kid;
      stack[sp].i = 0;
      sp++;
    }
    else {
      if (f->n == f->cap) {
        const size_t cap = f->cap ? f->cap * 2 : 16;
        const GTEXT_CSS_Value ** nv = gtext_allocator_realloc(&x->alloc, f->v, cap * sizeof *nv);
        if (!nv) {
          return GTEXT_CSS_E_OOM;
        }
        f->v = nv;
        f->cap = cap;
      }
      f->v[f->n++] = kid;
    }
  }
  return GTEXT_CSS_OK;
}

/// `animation-range` with no end: the end is the range name the start begins with, when it has one. True when it wrote it.
static bool range_end_from_start(anyctx * a, const GTEXT_CSS_Property_Id * ids, size_t nh, size_t (*assigned)[2], tbuf * out) {
  for (size_t h = 0; h < nh; h++) {
    if (!name_is(a->x->registry, gtext_css_registry_resolve_alias(a->x->registry, ids[h]), "animation-range-start") || assigned[h][1] <= assigned[h][0]) {
      continue;
    }
    const GTEXT_CSS_Value * first = a->l->v[assigned[h][0]];
    static const char * const names[] = {"entry", "exit", "cover", "contain", "entry-crossing", "exit-crossing"};
    for (size_t k = 0; k < sizeof names / sizeof *names; k++) {
      if (kw_is(first, names[k])) {
        tb_puts(out, names[k]);
        return true;
      }
    }
  }
  return false;
}

/// `background-position` is the grammar of <position>, not of two lists: one value is the horizontal (the vertical `center`), or the vertical when it
/// is `top` or `bottom`; two single values are the horizontal then the vertical, the other way round when the first is `top`/`bottom` or the second
/// `left`/`right`. Three and four values are read by the generic assignment. Rewrites `assigned` for the one and two value forms.
static void fix_position_layer(const anyctx * a, const GTEXT_CSS_Property_Id * ids, size_t (*assigned)[2]) {
  size_t xi = 2, yi = 2;
  for (size_t h = 0; h < 2; h++) {
    const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(a->x->registry, ids[h]);
    if (name_is(a->x->registry, id, "background-position-x")) {
      xi = h;
    }
    else if (name_is(a->x->registry, id, "background-position-y")) {
      yi = h;
    }
  }
  if (xi == 2 || yi == 2 || a->l->n < 1 || a->l->n > 2) {
    return;
  }
  const GTEXT_CSS_Value * v0 = a->l->v[0];
  const GTEXT_CSS_Value * v1 = a->l->n > 1 ? a->l->v[1] : NULL;
  const bool v0_vert = kw_is(v0, "top") || kw_is(v0, "bottom");
  if (!v1) {
    assigned[xi][0] = assigned[xi][1] = assigned[yi][0] = assigned[yi][1] = 0;
    if (v0_vert) {
      assigned[yi][1] = 1;
    }
    else {
      assigned[xi][1] = 1;
    }
    return;
  }
  const bool swap = v0_vert || kw_is(v1, "left") || kw_is(v1, "right");
  assigned[xi][0] = swap ? 1 : 0;
  assigned[xi][1] = swap ? 2 : 1;
  assigned[yi][0] = swap ? 0 : 1;
  assigned[yi][1] = swap ? 1 : 2;
}

/// `background` and `mask`: one <box> sets the origin and the clip both (Backgrounds 3, `background`; Masking 1, `mask`).
static void fix_box_pair(const anyctx * a, const GTEXT_CSS_Property_Id * ids, size_t nh, size_t (*assigned)[2], int * forced_origin) {
  *forced_origin = -1;
  size_t o = nh, c = nh;
  for (size_t h = 0; h < nh; h++) {
    const GTEXT_CSS_Property_Id id = gtext_css_registry_resolve_alias(a->x->registry, ids[h]);
    if (name_is(a->x->registry, id, "background-origin") || name_is(a->x->registry, id, "mask-origin")) {
      o = h;
    }
    else if (name_is(a->x->registry, id, "background-clip") || name_is(a->x->registry, id, "mask-clip")) {
      c = h;
    }
  }
  if (o == nh || c == nh) {
    return;
  }
  const bool has_o = assigned[o][1] > assigned[o][0], has_c = assigned[c][1] > assigned[c][0];
  if (has_o && !has_c && accepts((anyctx *)a, c, assigned[o][0], assigned[o][1]) > 0) {
    assigned[c][0] = assigned[o][0];
    assigned[c][1] = assigned[o][1];
  }
  else if (has_c && !has_o && accepts((anyctx *)a, o, assigned[c][0], assigned[c][1]) > 0) {
    assigned[o][0] = assigned[c][0];
    assigned[o][1] = assigned[c][1];
  }
  else if (has_c && !has_o && assigned[c][1] == assigned[c][0] + 1 && kw_is(a->l->v[assigned[c][0]], "border-area")) {
    *forced_origin = (int)o;   /* `border-area` is a clip only; the origin is then border-box (Backgrounds 4, `background`) */
  }
  else if (has_c && !has_o && assigned[c][1] == assigned[c][0] + 2 && kw_is(a->l->v[assigned[c][0]], "border-area")) {
    *forced_origin = (int)o;
  }
}

static GTEXT_CSS_Status split_any_layers(xctx * x, GTEXT_CSS_Property_Id sh, const GTEXT_CSS_Value * root, bool layered, size_t nl,
    const GTEXT_CSS_Property_Id * ids, size_t nresets, char ** txt) {
  const size_t nh = nl - nresets;
  if (nh > 60) {
    return x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a shorthand with more longhands than the split reads");
  }
  flat_layers fl;
  memset(&fl, 0, sizeof fl);
  size_t nlayers = 1;
  if (layered) {
    const GTEXT_CSS_Status fs = flatten_layers(x, root, &fl);
    if (fs != GTEXT_CSS_OK) {
      free_flat(x, &fl);
      return fs == GTEXT_CSS_E_LIMIT_LIST ? x_fail(x, fs, sh, "more layers than the split reads") : fs;
    }
    nlayers = fl.nb;
  }
  if (nlayers > 4096) {
    free_flat(x, &fl);
    return x_fail(x, GTEXT_CSS_E_LIMIT_LIST, sh, "more layers than the split reads");
  }
  unsigned long long final_only = 0;
  if (layered && name_is(x->registry, sh, "background")) {
    for (size_t h = 0; h < nh; h++) {
      if (name_is(x->registry, gtext_css_registry_resolve_alias(x->registry, ids[h]), "background-color")) {
        final_only |= 1ull << h;
      }
    }
  }
  tbuf * out = gtext_allocator_calloc(&x->alloc, nh ? nh : 1, sizeof *out);
  if (!out) {
    free_flat(x, &fl);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t h = 0; h < nh; h++) {
    tb_init(&out[h], &x->alloc);
  }
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  unsigned long long ever = 0;   /* the longhands some layer gave a component */
  size_t (*assigned)[2] = gtext_allocator_calloc(&x->alloc, nh ? nh : 1, sizeof *assigned);
  if (!assigned) {
    status = GTEXT_CSS_E_OOM;
  }
  int forced_origin = -1;   /* a longhand with no component that takes `border-box` all the same */
  for (size_t layer = 0; layer < nlayers && status == GTEXT_CSS_OK; layer++) {
    leaves_t * l = gtext_allocator_malloc(&x->alloc, sizeof *l);
    if (!l) {
      status = GTEXT_CSS_E_OOM;
      break;
    }
    bool got;
    if (layered) {
      const size_t from = fl.bound[layer], to = layer + 1 < fl.nb ? fl.bound[layer + 1] : fl.n;
      got = to - from <= MAX_LEAVES;
      if (got) {
        l->n = to - from;
        memcpy(l->v, fl.v + from, (to - from) * sizeof *l->v);
      }
    }
    else {
      got = leaves_of(root, l);
    }
    if (!got) {
      gtext_allocator_free(&x->alloc, l);
      status = x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "a layer with more components than the split reads");
      break;
    }
    anyctx a;
    memset(&a, 0, sizeof a);
    a.x = x;
    a.ids = ids;
    a.nh = nh;
    a.l = l;
    a.n = l->n;
    a.final_only = final_only;
    a.texts = gtext_allocator_calloc(&x->alloc, l->n ? l->n : 1, sizeof *a.texts);
    a.memo = gtext_allocator_calloc(&x->alloc, nh * (l->n + 1) * (l->n + 1) + 1, 1);
    if (!a.texts || !a.memo) {
      status = GTEXT_CSS_E_OOM;
    }
    else {
      status = assign_layer(&a, sh, layer + 1 == nlayers, assigned);
      forced_origin = -1;
      if (status == GTEXT_CSS_OK && layered && (name_is(x->registry, sh, "background") || name_is(x->registry, sh, "mask"))) {
        fix_box_pair(&a, ids, nh, assigned, &forced_origin);
      }
      if (status == GTEXT_CSS_OK && layered && nh == 2 && name_is(x->registry, sh, "background-position")) {
        fix_position_layer(&a, ids, assigned);
      }
      if (status == GTEXT_CSS_OK && x->status != GTEXT_CSS_OK) {
        status = x->status;   /* a check of the helpers above (a longhand's grammar asked about a run) ran out of memory */
      }
    }
    for (size_t h = 0; h < nh && status == GTEXT_CSS_OK; h++) {
      if (assigned && assigned[h][1] > assigned[h][0]) {
        ever |= 1ull << h;
      }
      if (layered && name_is(x->registry, sh, "box-shadow") && status == GTEXT_CSS_OK) {
        if (!x->dflt) {
          x->dflt = gtext_allocator_calloc(&x->alloc, nh * nlayers, 1);
          x->dflt_layers = nlayers;
          if (!x->dflt) {
            x->oom = true;
            status = GTEXT_CSS_E_OOM;
            break;
          }
        }
        x->dflt[h * nlayers + layer] = !(assigned[h][1] > assigned[h][0]);
      }
      if (final_only && !(layer + 1 == nlayers) && ((final_only >> h) & 1ull)) {
        continue; /* only the last layer carries it */
      }
      if (layer > 0 && !((final_only >> h) & 1ull)) {
        tb_puts(&out[h], ", ");
      }
      else if (layer > 0 && ((final_only >> h) & 1ull)) {
        /* the final-only longhand has one value, from the last layer */
      }
      if (assigned[h][1] > assigned[h][0]) {
        for (size_t i = assigned[h][0]; i < assigned[h][1]; i++) {
          if (i > assigned[h][0]) {
            tb_puts(&out[h], " ");
          }
          const char * t = leaf_text(&a, i);
          if (t) {
            tb_puts(&out[h], t);
          }
        }
      }
      else if ((int)h == forced_origin) {
        tb_puts(&out[h], "border-box");
      }
      else if (nh == 2 && name_is(x->registry, sh, "background-position")) {
        tb_puts(&out[h], "center");
      }
      else if (name_is(x->registry, sh, "animation-range") && name_is(x->registry, gtext_css_registry_resolve_alias(x->registry, ids[h]), "animation-range-end") &&
          range_end_from_start(&a, ids, nh, assigned, &out[h])) {
        /* an omitted end repeats the start's range name (Scroll-driven Animations 1, animation-range) */
      }
      else {
        bool oom = false;
        char * init = initial_layer_text(x, ids[h], &oom);
        if (!init) {
          status = oom ? GTEXT_CSS_E_OOM : x_fail(x, GTEXT_CSS_E_UNSUPPORTED, ids[h], "a longhand whose initial value is not written as a value");
        }
        else {
          tb_puts(&out[h], init);
          gtext_allocator_free(&x->alloc, init);
        }
      }
    }
    for (size_t i = 0; a.texts && i < l->n; i++) {
      gtext_allocator_free(&x->alloc, a.texts[i]);
    }
    gtext_allocator_free(&x->alloc, a.texts);
    gtext_allocator_free(&x->alloc, a.memo);
    gtext_allocator_free(&x->alloc, l);
  }
  /* animation-timeline is one value, not one for each layer, when no layer names a timeline (Scroll-driven Animations 1: `animation` resets it) */
  if (status == GTEXT_CSS_OK && layered && nlayers > 1 && name_is(x->registry, sh, "animation")) {
    for (size_t h = 0; h < nh; h++) {
      if (!((ever >> h) & 1ull) && name_is(x->registry, gtext_css_registry_resolve_alias(x->registry, ids[h]), "animation-timeline")) {
        bool oom = false;
        char * init = initial_layer_text(x, ids[h], &oom);
        if (init) {
          out[h].len = 0;
          tb_puts(&out[h], init);
          gtext_allocator_free(&x->alloc, init);
        }
        else if (oom) {
          status = GTEXT_CSS_E_OOM;
        }
      }
    }
  }
  /* a longhand no layer gave a component, for a shorthand that is not layered, is its initial value: absent */
  for (size_t h = 0; h < nh; h++) {
    if (status == GTEXT_CSS_OK) {
      txt[h] = tb_take(&out[h], NULL);
      if (!txt[h]) {
        status = GTEXT_CSS_E_OOM;
      }
      else if (!layered && txt[h][0] == '\0') {
        gtext_allocator_free(&x->alloc, txt[h]);
        txt[h] = NULL;
      }
    }
    else {
      tb_free(&out[h]);
    }
  }
  gtext_allocator_free(&x->alloc, assigned);
  gtext_allocator_free(&x->alloc, out);
  free_flat(x, &fl);
  return status;
}

/* ---- the driver ----------------------------------------------------------------------------------------------------------------- */

typedef struct {
  xctx * x;
  GTEXT_CSS_Property_Id sh;
  const GTEXT_CSS_Value_Set * set;
  size_t count;
  GTEXT_CSS_Status status;
  const GTEXT_CSS_Property_Id * ids;
  size_t nl;
  char ** txt;
} hook_ctx;

static GTEXT_CSS_Status hook_emit(void * user, size_t index, const GTEXT_CSS_Value * value) {
  hook_ctx * h = user;
  if (index >= h->nl) {
    return GTEXT_CSS_E_INVALID;
  }
  tbuf b;
  tb_init(&b, &h->x->alloc);
  css_exact_text(&b, value);
  gtext_allocator_free(&h->x->alloc, h->txt[index]);
  h->txt[index] = tb_take(&b, NULL);
  return h->txt[index] ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

static GTEXT_CSS_Status expand_one(xctx * x, qitem * qi) {
  GTEXT_CSS_Property_Value * pv = &qi->pv;
  const GTEXT_CSS_Property_Id sh = pv->property;
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(x->registry, sh, &info)) {
    return GTEXT_CSS_E_INVALID;
  }
  const size_t nlong = info.longhand_count;
  const size_t nreset = info.reset_longhand_count;
  const size_t nl = nlong + nreset;
  if (nlong > MAX_LONGHANDS_OF_ONE || nl > MAX_LONGHANDS_OF_ONE) {
    return x_fail(x, GTEXT_CSS_E_LIMIT_DECLARATIONS, sh, "a shorthand with more longhands than the library reads");
  }
  GTEXT_CSS_Property_Id * ids = gtext_allocator_calloc(&x->alloc, nl ? nl : 1, sizeof *ids);
  char ** txt = gtext_allocator_calloc(&x->alloc, nl ? nl : 1, sizeof *txt);
  if (!ids || !txt) {
    gtext_allocator_free(&x->alloc, ids);
    gtext_allocator_free(&x->alloc, txt);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < nlong; i++) {
    ids[i] = gtext_css_registry_longhand(x->registry, sh, i);
  }
  for (size_t i = 0; i < nreset; i++) {
    ids[nlong + i] = gtext_css_registry_reset_longhand(x->registry, sh, i);
  }
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  bool leaves_only_wide = pv->outcome == GTEXT_CSS_PROP_RESULT_WIDE || pv->outcome == GTEXT_CSS_PROP_RESULT_PENDING;
  if (leaves_only_wide) {
    for (size_t i = 0; i < nl && status == GTEXT_CSS_OK; i++) {
      const GTEXT_CSS_Property_Id target = gtext_css_registry_resolve_alias(x->registry, ids[i]);
      const bool reset = i >= nlong;
      if (is_shorthand_id(x->registry, target)) {
        GTEXT_CSS_Property_Value copy;
        memset(&copy, 0, sizeof copy);
        copy.outcome = pv->outcome;
        copy.wide = pv->wide;
        copy.set = gtext_css_value_set_retain(pv->set);
        copy.property = target;
        status = q_push(x, &copy, qi->from ? qi->from : sh, reset);
        if (status != GTEXT_CSS_OK) {
          gtext_css_property_value_free(&copy);
        }
      }
      else {
        status = list_append(x->list, target, pv->outcome, pv->wide, pv->set, qi->from ? qi->from : sh, reset);
      }
    }
    gtext_allocator_free(&x->alloc, ids);
    gtext_allocator_free(&x->alloc, txt);
    return status;
  }
  const GTEXT_CSS_Shorthand_Kind kind = gtext_css_shorthand_kind(x->registry, sh);
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(pv->set);
  switch (kind) {
  case GTEXT_CSS_SHORTHAND_BOX: status = split_box(x, sh, root, nlong, txt); break;
  case GTEXT_CSS_SHORTHAND_PAIR: status = split_pair(x, sh, root, nlong, txt); break;
  case GTEXT_CSS_SHORTHAND_COPY: status = split_copy(x, root, nlong, txt); break;
  case GTEXT_CSS_SHORTHAND_BORDER_RADIUS: status = split_border_radius(x, sh, root, nlong, txt); break;
  case GTEXT_CSS_SHORTHAND_FLEX: status = split_flex(x, sh, root, nlong, txt); break;
  case GTEXT_CSS_SHORTHAND_GRID_LINE: status = split_grid_line(x, sh, root, nlong, txt); break;
  case GTEXT_CSS_SHORTHAND_OFFSET: status = split_offset(x, sh, root, nlong, ids, txt); break;
  case GTEXT_CSS_SHORTHAND_GRID_TEMPLATE: status = split_grid(x, sh, root, nlong, ids, false, txt); break;
  case GTEXT_CSS_SHORTHAND_GRID: status = split_grid(x, sh, root, nlong, ids, true, txt); break;
  case GTEXT_CSS_SHORTHAND_TEXT_ALIGN: status = split_text_align(x, sh, root, nlong, txt); break;
  case GTEXT_CSS_SHORTHAND_FONT_SYNTHESIS: status = split_font_synthesis(x, sh, root, nlong, ids, txt); break;
  case GTEXT_CSS_SHORTHAND_FONT: status = split_font(x, sh, root, nl, ids, txt); break;
  case GTEXT_CSS_SHORTHAND_ANY:
    if (split_keyword_shorthand(x, sh, root, ids, nl, txt, &status)) {
      break;
    }
    status = split_any_layers(x, sh, root, false, nl, ids, nreset, txt);
    if (status == GTEXT_CSS_OK) {
      fix_list_style(x, sh, root, ids, nl, txt);
    }
    break;
  case GTEXT_CSS_SHORTHAND_LAYERS: status = split_any_layers(x, sh, root, true, nl, ids, nreset, txt); break;
  case GTEXT_CSS_SHORTHAND_HOOK: {
    GTEXT_CSS_Shorthand_Hook hook = NULL;
    void * huser = NULL;
    gtext_css_registry_shorthand_hook(x->registry, sh, &hook, &huser);
    hook_ctx hc = {x, sh, pv->set, 0, GTEXT_CSS_OK, ids, nl, txt};
    GTEXT_CSS_Longhand_Sink sink = {&hc, hook_emit};
    status = hook(huser, root, &sink);
    break;
  }
  default:
    status = x_fail(x, GTEXT_CSS_E_UNSUPPORTED, sh, "this shorthand has no rule for splitting its value (tools/css/shorthand_rules.tsv)");
    break;
  }
  if (status == GTEXT_CSS_OK && x->oom) {
    status = GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < nl && status == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Property_Id target = gtext_css_registry_resolve_alias(x->registry, ids[i]);
    const bool reset = i >= nlong;
    const GTEXT_CSS_Property_Id from = qi->from ? qi->from : sh;
    if (!txt[i]) {
      status = x_initial(x, target, from, reset);
      continue;
    }
    GTEXT_CSS_Property_Value lv;
    status = css_parse_longhand(x->registry, &x->alloc, target, txt[i], strlen(txt[i]), &lv);
    if (status != GTEXT_CSS_OK) {
      if (status == GTEXT_CSS_E_UNSUPPORTED) {
        status = x_fail(x, GTEXT_CSS_E_UNSUPPORTED, target, "the longhand's grammar cannot be decided");
      }
      break;
    }
    if (lv.outcome != GTEXT_CSS_PROP_RESULT_VALID) {
      gtext_css_property_value_free(&lv);
      status = x_fail(x, GTEXT_CSS_E_UNSUPPORTED, target, "the text the shorthand's value gave this longhand is not a value of it");
      break;
    }
    lv.property = target;
    if (x->dflt && i < nlong && lv.set) {
      /* the items the author left out are marked, in the set just made (nothing else holds it yet) */
      GTEXT_CSS_Value * root = (GTEXT_CSS_Value *)gtext_css_value_set_root(lv.set);
      GTEXT_CSS_Value * items = root;
      size_t count = 1;
      if (root && root->kind == GTEXT_CSS_VAL_LIST && root->separator == GTEXT_CSS_SEP_COMMA) {
        items = (GTEXT_CSS_Value *)root->children;
        count = root->child_count;
      }
      for (size_t k = 0; root && k < count && k < x->dflt_layers; k++) {
        if (x->dflt[i * x->dflt_layers + k]) {
          items[k].flags |= GTEXT_CSS_VFLAG_DEFAULTED;
        }
      }
    }
    status = x_deliver(x, target, &lv, from, reset);
  }
  gtext_allocator_free(&x->alloc, x->dflt);
  x->dflt = NULL;
  x->dflt_layers = 0;
  for (size_t i = 0; i < nl; i++) {
    gtext_allocator_free(&x->alloc, txt[i]);
  }
  gtext_allocator_free(&x->alloc, ids);
  gtext_allocator_free(&x->alloc, txt);
  return status;
}

GTEXT_CSS_Status gtext_css_shorthand_expand(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Property_Value * value,
    const GTEXT_Allocator * allocator, GTEXT_CSS_Longhand_List ** out, GTEXT_CSS_Shorthand_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->status = GTEXT_CSS_OK;
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!registry || !value || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if ((value->outcome != GTEXT_CSS_PROP_RESULT_VALID && value->outcome != GTEXT_CSS_PROP_RESULT_WIDE &&
      value->outcome != GTEXT_CSS_PROP_RESULT_PENDING) || !is_shorthand_id(registry, value->property)) {
    if (result) {
      result->status = GTEXT_CSS_E_INVALID;
      result->message = gtext_css_status_string(GTEXT_CSS_E_INVALID);
      result->detail = "the value is not a valid, wide or pending value of a shorthand";
    }
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * al = allocator ? allocator : css_registry_allocator(registry);
  xctx x;
  memset(&x, 0, sizeof x);
  x.registry = registry;
  x.alloc = *al;
  x.res = result;
  x.status = GTEXT_CSS_OK;
  GTEXT_CSS_Longhand_List * list = gtext_allocator_calloc(&x.alloc, 1, sizeof *list);
  if (!list) {
    return GTEXT_CSS_E_OOM;
  }
  list->alloc = x.alloc;
  x.list = list;
  /* the first work item is a view of the value (its set retained) */
  GTEXT_CSS_Property_Value first;
  memset(&first, 0, sizeof first);
  first.outcome = value->outcome;
  first.wide = value->wide;
  first.property = value->property;
  first.set = gtext_css_value_set_retain(value->set);
  GTEXT_CSS_Status status = q_push(&x, &first, 0, false);
  if (status != GTEXT_CSS_OK) {
    gtext_css_property_value_free(&first);
  }
  /* the queue is a stack of shorthands still to expand; the order of the longhands in the list is the order they were found */
  while (status == GTEXT_CSS_OK && x.qn) {
    qitem qi = x.queue[--x.qn];
    status = expand_one(&x, &qi);
    gtext_css_property_value_free(&qi.pv);
  }
  while (x.qn) {
    gtext_css_property_value_free(&x.queue[--x.qn].pv);
  }
  gtext_allocator_free(&x.alloc, x.queue);
  if (result) {
    result->work = x.steps + x.budget;
  }
  if (status != GTEXT_CSS_OK) {
    gtext_css_longhand_list_free(list);
    if (result && result->status == GTEXT_CSS_OK) {
      result->status = status;
      result->message = gtext_css_status_string(status);
    }
    return status;
  }
  *out = list;
  return GTEXT_CSS_OK;
}
