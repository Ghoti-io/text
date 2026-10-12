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
 * The HTML tree builder's state: the stack of open elements, the list of
 * active formatting elements, node insertion, the adoption agency algorithm,
 * and the public (internal-API) entry points. The insertion modes are in
 * html_tree_modes.c. See html_tree_internal.h for the contract.
 *
 * Source: WHATWG HTML, "Tree construction" (13.2.4 "Parse state", 13.2.6.1
 * "Creating and inserting nodes" with the appropriate place for inserting a node
 * and so foster parenting, 13.2.6.3 "Closing elements that have implied end tags",
 * and the adoption agency algorithm in 13.2.6.4.7), as served at
 * html.spec.whatwg.org/multipage/parsing.html (last updated 9 October 2026;
 * fetched 2026-10-11 and again 2026-10-12 for stages 3d and 3e).
 *
 * ## Foster parenting costs one comparison
 *
 * "The last template or table element in the stack of open elements" is the
 * larger of the topmost indexes of the names `template` and `table`, which the
 * per-name index already holds; inserting before the table or into its parent
 * is a linked-list edit. Text inserted there merges with the previous sibling
 * of the place. A document can alternate between the text before a table and the
 * text inside it, so a text node remembers the size of its own buffer (markup's
 * `cap_log2`) instead of the builder remembering the last one or two.
 *
 * ## The indexes that keep the common questions O(1)
 *
 * The stack of open elements is an array. For each name there is the index of
 * the topmost open element of that name (`names[id].topidx`), and each entry
 * remembers the one below it with the same name. Each entry also holds, for
 * each kind of scope, the index of the nearest boundary at or below it (its
 * "floor"), computed when it is pushed. So "does the stack have a `p` in
 * button scope" is "the topmost `p` is at or above the top entry's button
 * floor", and "any other end tag", the loops of the `li`, `dd` and `dt` start
 * tags, and the active formatting list's "the last `a` after the last marker"
 * are the same kind of comparison. Pushing and popping keep the indexes in step
 * in O(1). The few operations that change the middle of the stack (the adoption
 * agency algorithm, `</form>`, the head element after the head) unwind the
 * entries above the change, edit the array and derive them again, which is
 * O(entries above), the cost of the array shift itself.
 */

#include <ghoti.io/text/macros.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "html_tree_private.h"

/* ---- the tag table ------------------------------------------------------ */

const HtmlTagInfo gtext_html_tag_table[T__COUNT] = {
#define X(id, name, flags) {name, (uint32_t)(flags)},
    HTML_TAGS(X)
#undef X
};

/* ---- failure and errors ------------------------------------------------- */

bool ht_tree_fail(GTEXT_HTML_Tree * tb, GTEXT_HTML_TreeStatus status) {
  if (tb->status == GTEXT_HTML_TREE_OK) {
    tb->status = status;
  }
  return false;
}

bool ht_tree_markup_status(GTEXT_HTML_Tree * tb, GTEXT_Markup_Status status) {
  if (status == GTEXT_MARKUP_OK) {
    return true;
  }
  return ht_tree_fail(tb,
      status == GTEXT_MARKUP_E_OOM     ? GTEXT_HTML_TREE_E_OOM
      : status == GTEXT_MARKUP_E_LIMIT ? GTEXT_HTML_TREE_E_LIMIT_NODES
                                       : GTEXT_HTML_TREE_E_INVALID);
}

static void record_error(GTEXT_HTML_Tree * tb, int code, size_t line,
    size_t column, size_t offset) {
  if (tb->nerrors >= tb->max_errors) {
    tb->dropped++;
    return;
  }
  if (tb->nerrors == tb->cap_errors) {
    const size_t cap = tb->cap_errors ? tb->cap_errors * 2 : 16;
    GTEXT_HTML_TreeError * grown = gtext_allocator_realloc(
        &tb->alloc, tb->errors, cap * sizeof(*tb->errors));
    if (!grown) {
      ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
      return;
    }
    tb->errors = grown;
    tb->cap_errors = cap;
  }
  GTEXT_HTML_TreeError * e = &tb->errors[tb->nerrors++];
  e->code = code;
  e->line = line;
  e->column = column;
  e->offset = offset;
}

void ht_tree_error_raw(GTEXT_HTML_Tree * tb, int code) {
  const GTEXT_HTML_Token * t = tb->cur;
  record_error(tb, code, t ? t->line : 0, t ? t->column : 0, t ? t->offset : 0);
}

void ht_tree_error_at(GTEXT_HTML_Tree * tb, GTEXT_HTML_TreeErrorCode code, size_t line,
    size_t column, size_t offset) {
  record_error(tb, GTEXT_HTML_TREE_ERROR_BASE + (int)code, line, column, offset);
}

void ht_tree_error(GTEXT_HTML_Tree * tb, GTEXT_HTML_TreeErrorCode code) {
  ht_tree_error_raw(tb, GTEXT_HTML_TREE_ERROR_BASE + (int)code);
}

/* The tokenizer's errors raised since the last call, in the order raised. */
static void sync_tokenizer_errors(GTEXT_HTML_Tree * tb) {
  const size_t n = gtext_html_tokenizer_error_count(tb->tok);
  while (tb->tok_errors_seen < n) {
    const GTEXT_HTML_ParseError * e =
        gtext_html_tokenizer_error_at(tb->tok, tb->tok_errors_seen++);
    if (e) {
      record_error(tb, (int)e->code, e->line, e->column, e->offset);
    }
  }
}

/* ---- positions inside a run of characters -------------------------------- */

void ht_seg_cursor_init(SegCursor * c, const char * data, const GTEXT_HTML_TextSegment * segs,
    size_t nsegs, size_t line, size_t column, size_t offset) {
  c->data = data;
  c->segs = segs;
  c->nsegs = nsegs;
  c->next = 0;
  c->idx = 0;
  c->line = line;
  c->column = column;
  c->offset = offset;
}

/* Move to the byte `target` of the data: apply each segment when its byte is reached, and count the
   bytes between as the tokenizer does (a line feed starts a line; a character is one UTF-16 unit, two
   for a code point above U+FFFF, in the column of its first byte). */
void ht_seg_cursor_advance(SegCursor * c, size_t target) {
  for (;;) {
    while (c->next < c->nsegs && c->segs[c->next].index <= c->idx) {
      if (c->segs[c->next].index == c->idx) {
        c->line = c->segs[c->next].line;
        c->column = c->segs[c->next].column;
        c->offset = c->segs[c->next].offset;
      }
      c->next++;
    }
    if (c->idx >= target) {
      return;
    }
    const unsigned char b = (unsigned char)c->data[c->idx++];
    c->offset++;
    if (b == '\n') {
      c->line++;
      c->column = 1;
    }
    else if ((b & 0xC0) != 0x80) {
      c->column += b >= 0xF0 ? 2 : 1;
    }
  }
}

/* ---- names -------------------------------------------------------------- */

static int compare_known(const char * bytes, size_t len, const char * name) {
  const size_t nl = strlen(name);
  const int c = memcmp(bytes, name, len < nl ? len : nl);
  if (c != 0) {
    return c;
  }
  return len < nl ? -1 : len > nl ? 1 : 0;
}

/* The hash of a name in a namespace: a slot of the dynamic table is the pair (namespace, name), and the
   namespace is mixed into the seed, so the same bytes in two namespaces are two probe sequences. */
static size_t slot_hash(const GTEXT_HTML_Tree * tb, unsigned ns, const char * bytes, size_t len) {
  return (size_t)gtext_html_tokenizer_name_hash(
      tb->seed + (uint64_t)ns * 0x9E3779B97F4A7C15ULL, bytes, len);
}

static bool dyn_grow(GTEXT_HTML_Tree * tb) {
  const size_t cap = tb->dyn_cap ? tb->dyn_cap * 2 : 64;
  uint32_t * tab = gtext_allocator_calloc(&tb->alloc, cap, sizeof(uint32_t));
  if (!tab) {
    return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
  }
  for (size_t i = 0; i < tb->dyn_cap; i++) {
    if (tb->dyn_tab[i]) {
      const name_slot * s = &tb->names[tb->dyn_tab[i] - 1];
      size_t j = slot_hash(tb, s->ns, s->text, s->len) & (cap - 1);
      while (tab[j]) {
        j = (j + 1) & (cap - 1);
      }
      tab[j] = tb->dyn_tab[i];
    }
  }
  gtext_allocator_free(&tb->alloc, tb->dyn_tab);
  tb->dyn_tab = tab;
  tb->dyn_cap = cap;
  return true;
}

bool ht_names_lookup_ns(GTEXT_HTML_Tree * tb, unsigned ns, const char * bytes, size_t len,
    bool insert, uint32_t * id) {
  if (ns == NS_HTML) {
    /* The names the algorithms mention: a binary search of the sorted table. */
    size_t lo = 0;
    size_t hi = T__COUNT;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const int c = compare_known(bytes, len, gtext_html_tag_table[mid].name);
      if (c == 0) {
        *id = (uint32_t)mid;
        return true;
      }
      if (c < 0) {
        hi = mid;
      }
      else {
        lo = mid + 1;
      }
    }
  }
  if (tb->dyn_cap) {
    const size_t mask = tb->dyn_cap - 1;
    size_t j = slot_hash(tb, ns, bytes, len) & mask;
    while (tb->dyn_tab[j]) {
      const name_slot * s = &tb->names[tb->dyn_tab[j] - 1];
      if (s->ns == ns && s->len == len && memcmp(s->text, bytes, len) == 0) {
        *id = tb->dyn_tab[j] - 1;
        return true;
      }
      j = (j + 1) & mask;
    }
  }
  if (!insert) {
    *id = NONE_IDX;
    return true;
  }
  if ((tb->dyn_count + 1) * 2 > tb->dyn_cap && !dyn_grow(tb)) {
    return false;
  }
  if (tb->nnames == tb->cap_names) {
    const size_t cap = tb->cap_names * 2;
    name_slot * grown =
        gtext_allocator_realloc(&tb->alloc, tb->names, cap * sizeof(name_slot));
    if (!grown) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    }
    tb->names = grown;
    tb->cap_names = cap;
  }
  char * text = gtext_allocator_malloc(&tb->alloc, len + 1);
  if (!text) {
    return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
  }
  memcpy(text, bytes, len);
  text[len] = '\0';
  const uint32_t nid = (uint32_t)tb->nnames++;
  name_slot * s = &tb->names[nid];
  memset(s, 0, sizeof(*s));
  s->text = text;
  s->len = (uint32_t)len;
  s->topidx = NONE_IDX;
  s->dynamic = 1;
  s->ns = (uint8_t)ns;
  if (ns != NS_HTML) {
    s->flags = ht_foreign_flags(ns, bytes, len);
    s->disp = ns == NS_SVG ? ht_svg_tag_adjusted(bytes, len) : NULL;
  }
  const size_t mask = tb->dyn_cap - 1;
  size_t j = slot_hash(tb, ns, bytes, len) & mask;
  while (tb->dyn_tab[j]) {
    j = (j + 1) & mask;
  }
  tb->dyn_tab[j] = nid + 1;
  tb->dyn_count++;
  *id = nid;
  return true;
}

bool ht_names_lookup(GTEXT_HTML_Tree * tb, const char * bytes, size_t len,
    bool insert, uint32_t * id) {
  return ht_names_lookup_ns(tb, NS_HTML, bytes, len, insert, id);
}

/* ---- the stack of open elements ----------------------------------------- */

static void derive_entry(GTEXT_HTML_Tree * tb, size_t i) {
  stack_entry * e = &tb->stack[i];
  name_slot * s = &tb->names[e->id];
  e->prev_same = s->topidx;
  s->topidx = (uint32_t)i;
  const uint32_t f = e->flags;
  const bool special = (f & F_SPECIAL) != 0;
  const bool li_barrier = special && e->id != T_ADDRESS && e->id != T_DIV && e->id != T_P;
  const stack_entry * below = i > 0 ? &tb->stack[i - 1] : NULL;
  e->fl_gen = (f & F_SC_GEN) ? (uint32_t)i : below ? below->fl_gen : 0;
  e->fl_list = (f & (F_SC_GEN | F_SC_LIST)) ? (uint32_t)i : below ? below->fl_list : 0;
  e->fl_button = (f & (F_SC_GEN | F_SC_BUTTON)) ? (uint32_t)i : below ? below->fl_button : 0;
  e->fl_table = (f & F_SC_TABLE) ? (uint32_t)i : below ? below->fl_table : 0;
  e->fl_special = special ? (uint32_t)i : below ? below->fl_special : 0;
  e->fl_li = li_barrier ? (uint32_t)i : below ? below->fl_li : 0;
  e->fl_html = e->ns == NS_HTML ? (uint32_t)i : below ? below->fl_html : 0;
  if (e->fmt) {
    e->fmt->stack_idx = i;
  }
}

bool ht_stack_push(
    GTEXT_HTML_Tree * tb, HtNode * node, uint32_t id, fmt_entry * fmt) {
  if (tb->n >= tb->max_open) {
    return ht_tree_fail(tb, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  }
  if (tb->n == tb->cap) {
    const size_t cap = tb->cap ? tb->cap * 2 : 32;
    if (cap > SIZE_MAX / sizeof(stack_entry)) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    }
    stack_entry * grown =
        gtext_allocator_realloc(&tb->alloc, tb->stack, cap * sizeof(stack_entry));
    if (!grown) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    }
    tb->stack = grown;
    tb->cap = cap;
  }
  stack_entry * e = &tb->stack[tb->n];
  e->node = node;
  e->fmt = fmt;
  e->id = id;
  e->flags = (uint16_t)tb->names[id].flags;
  e->ns = tb->names[id].ns;
  e->removed = 0;
  derive_entry(tb, tb->n);
  tb->n++;
  tb->work++;
  return true;
}

void ht_stack_pop(GTEXT_HTML_Tree * tb) {
  stack_entry * e = &tb->stack[tb->n - 1];
  tb->names[e->id].topidx = e->prev_same;
  if (e->fmt) {
    e->fmt->stack_idx = SIZE_MAX;
  }
  tb->n--;
  tb->work++;
}

/* Undo the indexes of every entry from `from` up, ahead of editing the array
   under them; derive_range() makes them again. */
static void unwind_range(GTEXT_HTML_Tree * tb, size_t from) {
  for (size_t i = tb->n; i > from; i--) {
    stack_entry * e = &tb->stack[i - 1];
    tb->names[e->id].topidx = e->prev_same;
    if (e->fmt) {
      e->fmt->stack_idx = SIZE_MAX;
    }
  }
  tb->work += tb->n - from;
}

static void derive_range(GTEXT_HTML_Tree * tb, size_t from) {
  for (size_t i = from; i < tb->n; i++) {
    derive_entry(tb, i);
  }
  tb->work += tb->n - from;
}

void ht_stack_remove_at(GTEXT_HTML_Tree * tb, size_t idx) {
  unwind_range(tb, idx);
  memmove(&tb->stack[idx], &tb->stack[idx + 1],
      (tb->n - idx - 1) * sizeof(stack_entry));
  tb->n--;
  derive_range(tb, idx);
}

bool ht_stack_in_scope(const GTEXT_HTML_Tree * tb, uint32_t id, int kind) {
  if (id >= tb->nnames || tb->n == 0) {
    return false;
  }
  const uint32_t j = tb->names[id].topidx;
  if (j == NONE_IDX) {
    return false;
  }
  const stack_entry * top = &tb->stack[tb->n - 1];
  const uint32_t floor = kind == SCOPE_GEN      ? top->fl_gen
      : kind == SCOPE_LIST   ? top->fl_list
      : kind == SCOPE_BUTTON ? top->fl_button
                             : top->fl_table;
  return j >= floor;
}

void ht_stack_pop_until(GTEXT_HTML_Tree * tb, uint32_t id) {
  while (tb->n > 0) {
    const uint32_t popped = HT_STACK_TOP(tb).id;
    ht_stack_pop(tb);
    if (popped == id) {
      break;
    }
  }
}

bool ht_is_heading_id(uint32_t id) {
  return id >= T_H1 && id <= T_H6;
}

bool ht_stack_has_heading_in_scope(const GTEXT_HTML_Tree * tb) {
  for (uint32_t id = T_H1; id <= T_H6; id++) {
    if (ht_stack_in_scope(tb, id, SCOPE_GEN)) {
      return true;
    }
  }
  return false;
}

void ht_stack_pop_until_heading(GTEXT_HTML_Tree * tb) {
  while (tb->n > 0) {
    const uint32_t popped = HT_STACK_TOP(tb).id;
    ht_stack_pop(tb);
    if (ht_is_heading_id(popped)) {
      break;
    }
  }
}

/* "Generate all implied end tags thoroughly": the implied end tags and the table parts. */
void ht_generate_all_implied_end_tags_thoroughly(GTEXT_HTML_Tree * tb) {
  while (tb->n > 0) {
    const uint32_t id = HT_STACK_TOP(tb).id;
    if (!(HT_STACK_TOP(tb).flags & F_IMPLIED)
        && id != T_CAPTION && id != T_COLGROUP && id != T_TBODY && id != T_TD
        && id != T_TFOOT && id != T_TH && id != T_THEAD && id != T_TR) {
      break;
    }
    ht_stack_pop(tb);
  }
}

/* ---- the stack of template insertion modes ------------------------------ */

bool ht_tmpl_push(GTEXT_HTML_Tree * tb, InsertionMode m) {
  if (tb->ntmpl == tb->cap_tmpl) {
    const size_t cap = tb->cap_tmpl ? tb->cap_tmpl * 2 : 16;
    uint8_t * grown = gtext_allocator_realloc(&tb->alloc, tb->tmpl, cap);
    if (!grown) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    }
    tb->tmpl = grown;
    tb->cap_tmpl = cap;
  }
  tb->tmpl[tb->ntmpl++] = (uint8_t)m;
  tb->work++;
  return true;
}

void ht_tmpl_pop(GTEXT_HTML_Tree * tb) {
  if (tb->ntmpl > 0) {
    tb->ntmpl--;
  }
  tb->work++;
}

void ht_generate_implied_end_tags(GTEXT_HTML_Tree * tb, uint32_t except) {
  while (tb->n > 0 && (HT_STACK_TOP(tb).flags & F_IMPLIED)
      && HT_STACK_TOP(tb).id != except) {
    ht_stack_pop(tb);
  }
}

bool ht_close_p_element(GTEXT_HTML_Tree * tb) {
  ht_generate_implied_end_tags(tb, T_P);
  if (HT_STACK_TOP(tb).id != T_P) {
    ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
  }
  ht_stack_pop_until(tb, T_P);
  return tb->status == GTEXT_HTML_TREE_OK;
}

bool ht_close_p_if_in_button_scope(GTEXT_HTML_Tree * tb) {
  if (ht_stack_in_scope(tb, T_P, SCOPE_BUTTON)) {
    return ht_close_p_element(tb);
  }
  return true;
}

bool ht_any_other_end_tag(GTEXT_HTML_Tree * tb, uint32_t id) {
  if (id < tb->nnames) {
    const uint32_t j = tb->names[id].topidx;
    if (j != NONE_IDX && j >= HT_STACK_TOP(tb).fl_special) {
      ht_generate_implied_end_tags(tb, id);
      if (j != tb->n - 1) {
        ht_tree_error(tb, GTEXT_HTML_TREE_ERR_OPEN_ELEMENTS_AT_END_TAG);
      }
      while (tb->n > j) {
        ht_stack_pop(tb);
      }
      return tb->status == GTEXT_HTML_TREE_OK;
    }
  }
  ht_tree_error(tb, GTEXT_HTML_TREE_ERR_UNEXPECTED_END_TAG);
  return tb->status == GTEXT_HTML_TREE_OK;
}

/* ---- inserting nodes ---------------------------------------------------- */

static void place(GTEXT_HTML_Tree * tb, HtNode * node) {
  const GTEXT_HTML_Token * t = tb->cur;
  if (t) {
    GTEXT_Markup_Location where = {t->offset, t->line, t->column};
    gtext_markup_node_set_location(node, &where);
  }
}

/* An atom of the builder's, interned the first time it is needed. */
static markup_atom * lazy_atom(GTEXT_HTML_Tree * tb, markup_atom ** slot, const char * text) {
  if (!*slot) {
    *slot = markup_atom_intern(tb->doc, text, strlen(text));
    if (!*slot) {
      ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    }
  }
  return *slot;
}

/* The namespace atom of a namespace of the builder's, or NULL (HTML's is kept from the start). */
static markup_atom * element_ns_atom(GTEXT_HTML_Tree * tb, unsigned ns) {
  switch (ns) {
    case NS_SVG:
      return lazy_atom(tb, &tb->ns_svg, GTEXT_MARKUP_NS_SVG);
    case NS_MATH:
      return lazy_atom(tb, &tb->ns_math, GTEXT_MARKUP_NS_MATHML);
    default:
      return tb->ns_html;
  }
}

/* Add the attributes of a token to a foreign element: "adjust SVG attributes" or "adjust MathML
   attributes" for the element's namespace, then "adjust foreign attributes" (the namespaced ones). Then
   the check at the end of "create an element for a token": an `xmlns` attribute in the XMLNS namespace
   that is not the element's namespace, or an `xmlns:xlink` that is not the XLink namespace, is a parse
   error. */
static bool append_foreign_attributes(GTEXT_HTML_Tree * tb, HtNode * el, unsigned ns,
    const GTEXT_HTML_Attribute * attrs, size_t nattrs) {
  const char * want_xmlns = ns == NS_SVG ? GTEXT_MARKUP_NS_SVG : GTEXT_MARKUP_NS_MATHML;
  for (size_t i = 0; i < nattrs; i++) {
    const char * name = attrs[i].name;
    size_t len = attrs[i].name_length;
    markup_atom * ans = NULL;
    markup_atom * apfx = NULL;
    unsigned which = 0;
    if (ht_adjust_foreign_attribute(name, len, &which)) {
      /* The table: xlink:* in the XLink namespace, xml:lang and xml:space in the XML namespace, xmlns and
         xmlns:xlink in the XMLNS namespace (which = 1..3 names the namespace, the prefix is the one the
         table gives, the local name is what follows it). */
      switch (which) {
        case 1:
          ans = lazy_atom(tb, &tb->ns_xlink, GTEXT_MARKUP_NS_XLINK);
          apfx = lazy_atom(tb, &tb->pfx_xlink, "xlink");
          break;
        case 2:
          ans = lazy_atom(tb, &tb->ns_xml, GTEXT_MARKUP_NS_XML);
          apfx = lazy_atom(tb, &tb->pfx_xml, "xml");
          break;
        case 3:
          ans = lazy_atom(tb, &tb->ns_xmlns, GTEXT_MARKUP_NS_XMLNS);
          apfx = len > 5 ? lazy_atom(tb, &tb->pfx_xmlns, "xmlns") : NULL;
          break;
        default:
          break;
      }
      if (tb->status != GTEXT_HTML_TREE_OK) {
        return false;
      }
      const size_t skip = which == 3 ? (len > 5 ? 6 : 0) : (which == 1 ? 6 : 4);
      if (which == 3) {
        const bool is_xlink = len > 5;
        const char * v = attrs[i].value;
        const size_t vl = attrs[i].value_length;
        const char * w = is_xlink ? GTEXT_MARKUP_NS_XLINK : want_xmlns;
        if (vl != strlen(w) || memcmp(v, w, vl) != 0) {
          ht_tree_error(tb, GTEXT_HTML_TREE_ERR_XMLNS_MISMATCH);
        }
      }
      if (!ht_tree_markup_status(tb,
              markup_element_append_attribute(tb->doc, el, ans, apfx, name + skip,
                  len - skip, attrs[i].value, attrs[i].value_length))) {
        return false;
      }
      continue;
    }
    const char * adjusted = NULL;
    if (ns == NS_SVG) {
      adjusted = ht_svg_attribute_adjusted(name, len);
    }
    else if (ns == NS_MATH && len == 13 && memcmp(name, "definitionurl", 13) == 0) {
      adjusted = "definitionURL";
    }
    if (adjusted) {
      name = adjusted;
      len = strlen(adjusted);
    }
    if (!ht_tree_markup_status(tb,
            markup_element_append_attribute(tb->doc, el, NULL, NULL, name, len,
                attrs[i].value, attrs[i].value_length))) {
      return false;
    }
  }
  return true;
}

static bool make_element(GTEXT_HTML_Tree * tb, uint32_t id,
    const GTEXT_HTML_Attribute * attrs, size_t nattrs, HtNode ** out) {
  name_slot * s = &tb->names[id];
  if (!s->atom) {
    s->atom = markup_atom_intern(tb->doc, s->disp ? s->disp : s->text,
        s->disp ? strlen(s->disp) : s->len);
    if (!s->atom) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    }
  }
  markup_atom * ns_atom = element_ns_atom(tb, s->ns);
  if (!ns_atom) {
    return false;
  }
  HtNode * el = NULL;
  if (!ht_tree_markup_status(tb,
          markup_element_create_atoms(tb->doc, ns_atom, s->atom, &el))) {
    return false;
  }
  if (s->ns != NS_HTML) {
    if (!append_foreign_attributes(tb, el, s->ns, attrs, nattrs)) {
      return false;
    }
  }
  else {
    for (size_t i = 0; i < nattrs; i++) {
      if (!ht_tree_markup_status(tb,
              markup_element_append_attribute(tb->doc, el, NULL, NULL,
                  attrs[i].name, attrs[i].name_length, attrs[i].value,
                  attrs[i].value_length))) {
        return false;
      }
    }
  }
  if (id == T_TEMPLATE) {
    /* The template contents: a document fragment the element owns, made with it. */
    HtNode * content = NULL;
    if (!ht_tree_markup_status(
            tb, gtext_markup_element_ensure_template_content(el, &content))) {
      return false;
    }
  }
  place(tb, el);
  *out = el;
  return true;
}

/* The appropriate place for inserting a node (13.2.6.1), for the target at stack index `idx`
   (the current node, or the common ancestor of the adoption agency algorithm): with foster
   parenting enabled and the target a table, tbody, tfoot, thead or tr, before the last table or
   into the last template; and a template as the target means its contents. The last template or
   table of the stack is one comparison of the two names' topmost indexes. */
void ht_place_at(const GTEXT_HTML_Tree * tb, size_t idx, HtPlace * out) {
  const stack_entry * t = &tb->stack[idx];
  HtNode * target = t->node;
  HtNode * ref = NULL;
  bool tmpl = t->id == T_TEMPLATE;
  if (tb->foster
      && (t->id == T_TABLE || t->id == T_TBODY || t->id == T_TFOOT || t->id == T_THEAD
          || t->id == T_TR)) {
    const uint32_t lt = tb->names[T_TEMPLATE].topidx;
    const uint32_t lb = tb->names[T_TABLE].topidx;
    if (lt == NONE_IDX && lb == NONE_IDX) {
      /* The fragment case: the html element. */
      target = tb->stack[0].node;
      tmpl = false;
    }
    else if (lb == NONE_IDX || (lt != NONE_IDX && lt > lb)) {
      target = tb->stack[lt].node;
      tmpl = true;
    }
    else {
      HtNode * table = tb->stack[lb].node;
      if (table->parent) {
        target = table->parent;
        ref = table;
        tmpl = false;
      }
      else {
        target = tb->stack[lb - 1].node;
        tmpl = tb->stack[lb - 1].id == T_TEMPLATE;
      }
    }
  }
  if (tmpl) {
    target = ((markup_element *)target)->content;
  }
  out->parent = target;
  out->ref = ref;
}

static bool insert_element_at(GTEXT_HTML_Tree * tb, HtPlace at, uint32_t id,
    const GTEXT_HTML_Attribute * attrs, size_t nattrs, HtNode ** out) {
  if (tb->n >= tb->max_open) {
    return ht_tree_fail(tb, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  }
  HtNode * el = NULL;
  if (!make_element(tb, id, attrs, nattrs, &el)) {
    return false;
  }
  if (at.ref) {
    markup_insert_unchecked(at.parent, el, at.ref);
  }
  else {
    markup_append_unchecked(at.parent, el);
  }
  if (!ht_stack_push(tb, el, id, NULL)) {
    return false;
  }
  if (out) {
    *out = el;
  }
  return true;
}

bool ht_insert_element_into(GTEXT_HTML_Tree * tb, HtNode * parent, uint32_t id,
    const GTEXT_HTML_Attribute * attrs, size_t nattrs, HtNode ** out) {
  HtPlace at = {parent, NULL};
  return insert_element_at(tb, at, id, attrs, nattrs, out);
}

bool ht_insert_element(GTEXT_HTML_Tree * tb, const Tk * tk, HtNode ** out) {
  HtPlace at;
  ht_place_at(tb, tb->n - 1, &at);
  return insert_element_at(tb, at, tk->id, tk->attrs, tk->nattrs, out);
}

bool ht_insert_foreign(GTEXT_HTML_Tree * tb, const Tk * tk, uint32_t id, HtNode ** out) {
  HtPlace at;
  ht_place_at(tb, tb->n - 1, &at);
  HtNode * el = NULL;
  if (!insert_element_at(tb, at, id, tk->attrs, tk->nattrs, &el)) {
    return false;
  }
  /* A MathML annotation-xml element is an HTML integration point when its start tag had an `encoding`
     attribute that is "text/html" or "application/xhtml+xml" (ASCII case-insensitively): the token's
     attribute, as it came, not the element's. */
  const name_slot * s = &tb->names[id];
  if (s->flags & F_ANNOT) {
    for (size_t i = 0; i < tk->nattrs; i++) {
      const GTEXT_HTML_Attribute * a = &tk->attrs[i];
      if (a->name_length == 8 && memcmp(a->name, "encoding", 8) == 0
          && (ht_ci_equal(a->value, a->value_length, "text/html")
              || ht_ci_equal(a->value, a->value_length, "application/xhtml+xml"))) {
        HT_STACK_TOP(tb).flags |= F_HIP;
      }
    }
  }
  if (out) {
    *out = el;
  }
  return true;
}

bool ht_insert_element_bare(GTEXT_HTML_Tree * tb, uint32_t id, HtNode ** out) {
  HtPlace at;
  ht_place_at(tb, tb->n - 1, &at);
  return insert_element_at(tb, at, id, NULL, 0, out);
}

bool ht_pop_current_acknowledge(GTEXT_HTML_Tree * tb, const Tk * tk) {
  (void)tk;
  ht_stack_pop(tb);
  tb->ack_self_closing = true;
  return true;
}

bool ht_insert_text(GTEXT_HTML_Tree * tb, const char * data, size_t length) {
  if (length == 0) {
    return true;
  }
  HtPlace at;
  ht_place_at(tb, tb->n - 1, &at);
  HtNode * last = at.ref ? at.ref->prev : at.parent->last_child;
  if (last && last->kind == GTEXT_MARKUP_KIND_TEXT) {
    size_t cap = 0; /* the node knows how big its buffer is */
    return ht_tree_markup_status(
        tb, markup_text_append(tb->doc, last, &cap, data, length));
  }
  HtNode * text = NULL;
  if (!ht_tree_markup_status(
          tb, gtext_markup_create_text(tb->doc, data, length, &text))) {
    return false;
  }
  if (at.ref) {
    markup_insert_unchecked(at.parent, text, at.ref);
  }
  else {
    markup_append_unchecked(at.parent, text);
  }
  place(tb, text);
  return true;
}

static bool insert_comment_at(
    GTEXT_HTML_Tree * tb, HtPlace at, const GTEXT_HTML_Token * t) {
  HtNode * c = NULL;
  if (!ht_tree_markup_status(tb,
          gtext_markup_create_comment(tb->doc, t->data, t->data_length, &c))) {
    return false;
  }
  if (at.ref) {
    markup_insert_unchecked(at.parent, c, at.ref);
  }
  else {
    markup_append_unchecked(at.parent, c);
  }
  place(tb, c);
  return true;
}

bool ht_insert_comment_into(
    GTEXT_HTML_Tree * tb, HtNode * parent, const GTEXT_HTML_Token * t) {
  HtPlace at = {parent, NULL};
  return insert_comment_at(tb, at, t);
}

bool ht_insert_comment(GTEXT_HTML_Tree * tb, const GTEXT_HTML_Token * t) {
  HtPlace at;
  ht_place_at(tb, tb->n - 1, &at);
  return insert_comment_at(tb, at, t);
}

static bool insert_pi_at(GTEXT_HTML_Tree * tb, HtPlace at, const GTEXT_HTML_Token * t) {
  HtNode * n = NULL;
  if (t->name_length == 0) {
    /* The tokenizer never makes one; a comment is the safe reading. */
    return insert_comment_at(tb, at, t);
  }
  char * target = gtext_allocator_malloc(&tb->alloc, t->name_length + 1);
  if (!target) {
    return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
  }
  memcpy(target, t->name, t->name_length);
  target[t->name_length] = '\0';
  const GTEXT_Markup_Status st = gtext_markup_create_processing_instruction(
      tb->doc, target, t->data, t->data_length, &n);
  gtext_allocator_free(&tb->alloc, target);
  if (!ht_tree_markup_status(tb, st)) {
    return false;
  }
  if (at.ref) {
    markup_insert_unchecked(at.parent, n, at.ref);
  }
  else {
    markup_append_unchecked(at.parent, n);
  }
  place(tb, n);
  return true;
}

bool ht_insert_pi_into(
    GTEXT_HTML_Tree * tb, HtNode * parent, const GTEXT_HTML_Token * t) {
  HtPlace at = {parent, NULL};
  return insert_pi_at(tb, at, t);
}

bool ht_insert_pi(GTEXT_HTML_Tree * tb, const GTEXT_HTML_Token * t) {
  HtPlace at;
  ht_place_at(tb, tb->n - 1, &at);
  return insert_pi_at(tb, at, t);
}

/* The attribute index of `element`, built from its attributes if it is not the one the slot holds. */
static size_t name_slot_hash(const GTEXT_HTML_Tree * tb, const char * s, size_t n) {
  return (size_t)gtext_html_tokenizer_name_hash(tb->seed, s, n);
}

static bool index_add(GTEXT_HTML_Tree * tb, attr_index * ix, const markup_element * e, size_t i) {
  if ((ix->count + 1) * 2 > ix->cap) {
    const size_t cap = ix->cap ? ix->cap * 2 : 64;
    uint32_t * slots = gtext_allocator_calloc(&tb->alloc, cap, sizeof(uint32_t));
    if (!slots) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    }
    for (size_t k = 0; k < ix->cap; k++) {
      if (ix->slots[k]) {
        const markup_attr * a = &e->attrs[ix->slots[k] - 1];
        size_t j = name_slot_hash(tb, a->local->text, a->local->length) & (cap - 1);
        while (slots[j]) {
          j = (j + 1) & (cap - 1);
        }
        slots[j] = ix->slots[k];
      }
    }
    gtext_allocator_free(&tb->alloc, ix->slots);
    ix->slots = slots;
    ix->cap = cap;
  }
  const markup_attr * a = &e->attrs[i];
  size_t j = name_slot_hash(tb, a->local->text, a->local->length) & (ix->cap - 1);
  while (ix->slots[j]) {
    j = (j + 1) & (ix->cap - 1);
  }
  ix->slots[j] = (uint32_t)(i + 1);
  ix->count++;
  return true;
}

static attr_index * index_for(GTEXT_HTML_Tree * tb, HtNode * element) {
  attr_index * slot = tb->merge_index[0].el == element ? &tb->merge_index[0]
      : tb->merge_index[1].el == element ? &tb->merge_index[1] : NULL;
  if (slot) {
    return slot;
  }
  slot = tb->merge_index[0].el == NULL ? &tb->merge_index[0] : &tb->merge_index[1];
  gtext_allocator_free(&tb->alloc, slot->slots);
  memset(slot, 0, sizeof(*slot));
  slot->el = element;
  slot->cap = 64;
  slot->slots = gtext_allocator_calloc(&tb->alloc, slot->cap, sizeof(uint32_t));
  if (!slot->slots) {
    ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
    return NULL;
  }
  const markup_element * e = (const markup_element *)element;
  for (size_t i = 0; i < e->attr_count; i++) {
    if (!index_add(tb, slot, e, i)) {
      return NULL;
    }
  }
  return slot;
}

/* Add the attributes of a token that the element does not have already: O(1) for each, however many
   start tags have added before. The standard puts no limit on how many an element ends up with. */
bool ht_merge_attributes(GTEXT_HTML_Tree * tb, HtNode * element,
    const GTEXT_HTML_Attribute * attrs, size_t n) {
  if (n == 0) {
    return true;
  }
  attr_index * ix = index_for(tb, element);
  if (!ix) {
    return false;
  }
  markup_element * e = (markup_element *)element;
  for (size_t i = 0; i < n; i++) {
    tb->work++;
    bool present = false;
    size_t j = name_slot_hash(tb, attrs[i].name, attrs[i].name_length) & (ix->cap - 1);
    while (ix->slots[j]) {
      const markup_attr * a = &e->attrs[ix->slots[j] - 1];
      if (!a->ns && a->local->length == attrs[i].name_length
          && memcmp(a->local->text, attrs[i].name, attrs[i].name_length) == 0) {
        present = true;
        break;
      }
      j = (j + 1) & (ix->cap - 1);
    }
    if (present) {
      continue;
    }
    if (!ht_tree_markup_status(tb,
            markup_element_append_attribute(tb->doc, element, NULL, NULL,
                attrs[i].name, attrs[i].name_length, attrs[i].value,
                attrs[i].value_length))
        || !index_add(tb, ix, e, e->attr_count - 1)) {
      return false;
    }
  }
  return true;
}

bool ht_insert_doctype(GTEXT_HTML_Tree * tb, const GTEXT_HTML_Token * t) {
  const size_t nl = t->has_name ? t->name_length : 0;
  const size_t pl = t->has_public_id ? t->public_id_length : 0;
  const size_t sl = t->has_system_id ? t->system_id_length : 0;
  char * buf = gtext_allocator_malloc(&tb->alloc, nl + pl + sl + 3);
  if (!buf) {
    return ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
  }
  char * name = buf;
  char * pub = buf + nl + 1;
  char * sys = buf + nl + pl + 2;
  if (nl) {
    memcpy(name, t->name, nl);
  }
  name[nl] = '\0';
  if (pl) {
    memcpy(pub, t->public_id, pl);
  }
  pub[pl] = '\0';
  if (sl) {
    memcpy(sys, t->system_id, sl);
  }
  sys[sl] = '\0';
  HtNode * d = NULL;
  const GTEXT_Markup_Status st = gtext_markup_create_doctype(tb->doc, name,
      t->has_public_id ? pub : NULL, t->has_system_id ? sys : NULL, &d);
  gtext_allocator_free(&tb->alloc, buf);
  if (!ht_tree_markup_status(tb, st)) {
    return false;
  }
  markup_append_unchecked(gtext_markup_document_root(tb->doc), d);
  place(tb, d);
  return true;
}

void ht_set_mode_from_doctype(GTEXT_HTML_Tree * tb, const GTEXT_HTML_Token * t) {
  const int mode = ht_tree_quirks_mode(t->has_name, t->name, t->name_length,
      t->has_public_id, t->public_id, t->public_id_length, t->has_system_id,
      t->system_id, t->system_id_length, t->force_quirks);
  gtext_markup_document_set_mode(tb->doc, (GTEXT_Markup_Mode)mode);
}

/* ---- the list of active formatting elements ----------------------------- */

static fmt_entry * fmt_alloc(GTEXT_HTML_Tree * tb) {
  if (!tb->fmt_free) {
    enum { BLOCK = 32 };
    if (tb->fmt_nblocks == tb->fmt_block_cap) {
      const size_t cap = tb->fmt_block_cap ? tb->fmt_block_cap * 2 : 8;
      void ** grown = gtext_allocator_realloc(
          &tb->alloc, tb->fmt_blocks, cap * sizeof(void *));
      if (!grown) {
        ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
        return NULL;
      }
      tb->fmt_blocks = grown;
      tb->fmt_block_cap = cap;
    }
    fmt_entry * block = gtext_allocator_calloc(&tb->alloc, BLOCK, sizeof(fmt_entry));
    if (!block) {
      ht_tree_fail(tb, GTEXT_HTML_TREE_E_OOM);
      return NULL;
    }
    tb->fmt_blocks[tb->fmt_nblocks++] = block;
    for (size_t i = 0; i < BLOCK; i++) {
      block[i].next = tb->fmt_free;
      tb->fmt_free = &block[i];
    }
  }
  fmt_entry * e = tb->fmt_free;
  tb->fmt_free = e->next;
  memset(e, 0, sizeof(*e));
  e->stack_idx = SIZE_MAX;
  return e;
}

static void fmt_release(GTEXT_HTML_Tree * tb, fmt_entry * e) {
  memset(e, 0, sizeof(*e));
  e->next = tb->fmt_free;
  tb->fmt_free = e;
}

static void list_link_after(GTEXT_HTML_Tree * tb, fmt_entry * pos, fmt_entry * e) {
  e->prev = pos;
  e->next = pos ? pos->next : tb->fmt_head;
  if (e->next) {
    e->next->prev = e;
  }
  else {
    tb->fmt_tail = e;
  }
  if (pos) {
    pos->next = e;
  }
  else {
    tb->fmt_head = e;
  }
}

static void list_unlink(GTEXT_HTML_Tree * tb, fmt_entry * e) {
  if (e->prev) {
    e->prev->next = e->next;
  }
  else {
    tb->fmt_head = e->next;
  }
  if (e->next) {
    e->next->prev = e->prev;
  }
  else {
    tb->fmt_tail = e->prev;
  }
  e->prev = NULL;
  e->next = NULL;
}

static void chain_unlink(GTEXT_HTML_Tree * tb, fmt_entry * e) {
  if (e->same_prev) {
    e->same_prev->same_next = e->same_next;
  }
  if (e->same_next) {
    e->same_next->same_prev = e->same_prev;
  }
  else {
    tb->names[e->id].fmt_last = e->same_prev;
  }
  e->same_prev = NULL;
  e->same_next = NULL;
}

/* Put an element entry that is in the list, but in no chain, into its name's chain at
   the place the list order gives it. */
static void chain_insert(GTEXT_HTML_Tree * tb, fmt_entry * e) {
  fmt_entry * pred = e->prev;
  while (pred && !(pred->kind == FE_ELEMENT && pred->id == e->id)) {
    pred = pred->prev;
    tb->work++;
  }
  fmt_entry * succ = e->next;
  while (succ && !(succ->kind == FE_ELEMENT && succ->id == e->id)) {
    succ = succ->next;
    tb->work++;
  }
  e->same_prev = pred;
  e->same_next = succ;
  if (pred) {
    pred->same_next = e;
  }
  if (succ) {
    succ->same_prev = e;
  }
  else {
    tb->names[e->id].fmt_last = e;
  }
}

bool ht_fmt_after_marker(const GTEXT_HTML_Tree * tb, const fmt_entry * e) {
  return !tb->last_marker || e->seq > tb->last_marker->seq;
}

/* Take an entry out of the list for good. */
void ht_fmt_remove(GTEXT_HTML_Tree * tb, fmt_entry * e) {
  list_unlink(tb, e);
  if (e->kind == FE_ELEMENT) {
    chain_unlink(tb, e);
  }
  if (e->stack_idx != SIZE_MAX) {
    tb->stack[e->stack_idx].fmt = NULL;
  }
  fmt_release(tb, e);
}

bool ht_fmt_push_marker(GTEXT_HTML_Tree * tb) {
  fmt_entry * e = fmt_alloc(tb);
  if (!e) {
    return false;
  }
  e->kind = FE_MARKER;
  e->seq = ++tb->fmt_seq;
  e->prev_marker = tb->last_marker;
  tb->last_marker = e;
  list_link_after(tb, tb->fmt_tail, e);
  return true;
}

void ht_fmt_clear_to_marker(GTEXT_HTML_Tree * tb) {
  while (tb->fmt_tail) {
    fmt_entry * e = tb->fmt_tail;
    tb->work++;
    if (e->kind == FE_MARKER) {
      tb->last_marker = e->prev_marker;
      list_unlink(tb, e);
      fmt_release(tb, e);
      return;
    }
    ht_fmt_remove(tb, e);
  }
}

static uint64_t mix64(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

static uint64_t fnv(uint64_t h, const char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

/* The same value for the same set of attributes in any order. */
static uint64_t attrs_hash(const GTEXT_HTML_Attribute * attrs, size_t n) {
  uint64_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t h = fnv(0xcbf29ce484222325ULL, attrs[i].name, attrs[i].name_length);
    h = fnv(h * 31 + 1, attrs[i].value, attrs[i].value_length);
    sum += mix64(h);
  }
  return sum;
}

/* Do the attributes the element was created with equal this token's, as sets? */
static bool attrs_match(const HtNode * element, const GTEXT_HTML_Attribute * attrs, size_t n) {
  const markup_element * e = (const markup_element *)element;
  if (e->attr_count != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    bool found = false;
    for (size_t k = 0; k < n && !found; k++) {
      const markup_attr * a = &e->attrs[k];
      found = !a->ns && a->local->length == attrs[i].name_length
          && memcmp(a->local->text, attrs[i].name, attrs[i].name_length) == 0
          && a->value_length == attrs[i].value_length
          && (a->value_length == 0
              || memcmp(a->value, attrs[i].value, a->value_length) == 0);
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

/* Push the element just inserted (the top of the stack) onto the list, with the
   Noah's Ark clause: three entries of the same name and attributes after the last
   marker are the most the list keeps. */
bool ht_fmt_push_element(GTEXT_HTML_Tree * tb, const Tk * tk) {
  const uint64_t hash = attrs_hash(tk->attrs, tk->nattrs);
  size_t count = 0;
  fmt_entry * earliest = NULL;
  for (fmt_entry * e = tb->names[tk->id].fmt_last; e && ht_fmt_after_marker(tb, e);
       e = e->same_prev) {
    tb->work++;
    if (e->nattrs == tk->nattrs && e->attr_hash == hash
        && attrs_match(e->el, tk->attrs, tk->nattrs)) {
      count++;
      earliest = e;
    }
  }
  if (count >= 3) {
    ht_fmt_remove(tb, earliest);
  }
  fmt_entry * e = fmt_alloc(tb);
  if (!e) {
    return false;
  }
  e->kind = FE_ELEMENT;
  e->el = HT_STACK_TOP(tb).node;
  e->id = tk->id;
  e->nattrs = (uint32_t)tk->nattrs;
  e->attr_hash = hash;
  e->seq = ++tb->fmt_seq;
  e->serial = ++tb->fmt_serial;
  list_link_after(tb, tb->fmt_tail, e);
  chain_insert(tb, e);
  e->stack_idx = tb->n - 1;
  HT_STACK_TOP(tb).fmt = e;
  return true;
}

/* Reopen the formatting elements that are in the list and no longer in the stack. */
bool ht_fmt_reconstruct(GTEXT_HTML_Tree * tb) {
  fmt_entry * e = tb->fmt_tail;
  if (!e || e->kind == FE_MARKER || e->stack_idx != SIZE_MAX) {
    return true;
  }
  while (e->prev && e->prev->kind == FE_ELEMENT && e->prev->stack_idx == SIZE_MAX) {
    e = e->prev;
    tb->work++;
  }
  for (; e; e = e->next) {
    tb->work++;
    if (tb->n >= tb->max_open) {
      return ht_tree_fail(tb, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
    }
    HtNode * fresh = NULL;
    if (!ht_tree_markup_status(
            tb, markup_element_clone_shallow(tb->doc, e->el, &fresh))) {
      return false;
    }
    HtPlace at;
    ht_place_at(tb, tb->n - 1, &at);
    markup_insert_unchecked(at.parent, fresh, at.ref);
    e->el = fresh;
    if (!ht_stack_push(tb, fresh, e->id, e)) {
      return false;
    }
  }
  return true;
}

/* ---- the adoption agency algorithm -------------------------------------- */

bool ht_adoption_agency(GTEXT_HTML_Tree * tb, uint32_t subject) {
  if (tb->n > 0 && HT_STACK_TOP(tb).id == subject && HT_STACK_TOP(tb).fmt == NULL) {
    ht_stack_pop(tb);
    return true;
  }
  for (int outer = 0; outer < 8; outer++) {
    fmt_entry * fe = subject < tb->nnames ? tb->names[subject].fmt_last : NULL;
    if (!fe || !ht_fmt_after_marker(tb, fe)) {
      return ht_any_other_end_tag(tb, subject);
    }
    if (fe->stack_idx == SIZE_MAX) {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_MISNESTED_FORMATTING);
      ht_fmt_remove(tb, fe);
      return tb->status == GTEXT_HTML_TREE_OK;
    }
    const size_t idx = fe->stack_idx;
    const size_t n = tb->n;
    if (idx < tb->stack[n - 1].fl_gen) {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_MISNESTED_FORMATTING);
      return tb->status == GTEXT_HTML_TREE_OK;
    }
    if (idx != n - 1) {
      ht_tree_error(tb, GTEXT_HTML_TREE_ERR_MISNESTED_FORMATTING);
    }
    size_t fb = SIZE_MAX;
    for (size_t i = idx + 1; i < n; i++) {
      tb->work++;
      if (tb->stack[i].flags & F_SPECIAL) {
        fb = i;
        break;
      }
    }
    if (fb == SIZE_MAX) {
      while (tb->n > idx) {
        ht_stack_pop(tb);
      }
      ht_fmt_remove(tb, fe);
      return tb->status == GTEXT_HTML_TREE_OK;
    }

    HtNode * fb_node = tb->stack[fb].node;
    fmt_entry * bm = &tb->bookmark;
    memset(bm, 0, sizeof(*bm));
    bm->kind = FE_BOOKMARK;
    list_link_after(tb, fe, bm);

    HtNode * last_node = fb_node;
    size_t node_idx = fb;
    unsigned inner = 0;
    for (;;) {
      inner++;
      node_idx--;
      tb->work++;
      if (node_idx == idx) {
        break;
      }
      stack_entry * se = &tb->stack[node_idx];
      if (inner > 3 && se->fmt) {
        ht_fmt_remove(tb, se->fmt);
      }
      if (!se->fmt) {
        se->removed = 1;
        continue;
      }
      HtNode * fresh = NULL;
      if (!ht_tree_markup_status(
              tb, markup_element_clone_shallow(tb->doc, se->node, &fresh))) {
        return false;
      }
      fmt_entry * le = se->fmt;
      le->el = fresh;
      se->node = fresh;
      if (last_node == fb_node) {
        list_unlink(tb, bm);
        list_link_after(tb, le, bm);
      }
      markup_insert_unchecked(fresh, last_node, NULL);
      last_node = fresh;
    }
    {
      /* "Insert lastNode at the appropriate place for inserting a node, given the common
         ancestor": with foster parenting enabled that can be before a table. */
      HtPlace at;
      ht_place_at(tb, idx - 1, &at);
      if (at.ref == last_node) {
        at.ref = last_node->next;
      }
      markup_insert_unchecked(at.parent, last_node, at.ref);
    }

    HtNode * nfe = NULL;
    if (!ht_tree_markup_status(
            tb, markup_element_clone_shallow(tb->doc, fe->el, &nfe))) {
      return false;
    }
    tb->work += 1;
    for (const HtNode * c = fb_node->first_child; c; c = c->next) {
      tb->work++;
    }
    markup_move_children_unchecked(fb_node, nfe);
    markup_insert_unchecked(fb_node, nfe, NULL);

    /* The list: the formatting element's entry becomes the new element's, at
       the bookmark. */
    list_unlink(tb, fe);
    chain_unlink(tb, fe);
    fe->el = nfe;
    fe->seq = ++tb->fmt_seq;
    fe->serial = ++tb->fmt_serial;
    list_link_after(tb, bm->prev, fe);
    list_unlink(tb, bm);
    chain_insert(tb, fe);

    /* The stack: the formatting element and the elements this loop removed go,
       and the new element goes in immediately below the furthest block. */
    unwind_range(tb, idx);
    size_t w = idx;
    for (size_t r = idx + 1; r < n; r++) {
      if (tb->stack[r].removed) {
        tb->stack[r].removed = 0;
        continue;
      }
      tb->stack[w++] = tb->stack[r];
      if (r == fb) {
        stack_entry * ne = &tb->stack[w++];
        memset(ne, 0, sizeof(*ne));
        ne->node = nfe;
        ne->fmt = fe;
        ne->id = fe->id;
        ne->flags = (uint16_t)tb->names[fe->id].flags;
      }
    }
    tb->n = w;
    derive_range(tb, idx);
  }
  return true;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void release_atoms(GTEXT_HTML_Tree * tb) {
  if (!tb->doc) {
    return;
  }
  for (size_t i = 0; i < tb->nnames; i++) {
    if (tb->names[i].atom) {
      markup_atom_release(tb->doc, tb->names[i].atom);
      tb->names[i].atom = NULL;
    }
  }
  markup_atom ** const others[] = {&tb->ns_html, &tb->ns_svg, &tb->ns_math, &tb->ns_xlink,
      &tb->ns_xml, &tb->ns_xmlns, &tb->pfx_xlink, &tb->pfx_xml, &tb->pfx_xmlns};
  for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
    if (*others[i]) {
      markup_atom_release(tb->doc, *others[i]);
      *others[i] = NULL;
    }
  }
}

void gtext_html_tree_free(GTEXT_HTML_Tree * tb) {
  if (!tb) {
    return;
  }
  const GTEXT_Allocator alloc = tb->alloc;
  gtext_html_tokenizer_free(tb->tok);
  release_atoms(tb);
  if (tb->doc && !tb->doc_taken) {
    gtext_markup_document_free(tb->doc);
  }
  for (size_t i = T__COUNT; i < tb->nnames; i++) {
    gtext_allocator_free(&alloc, (void *)tb->names[i].text);
  }
  gtext_allocator_free(&alloc, tb->names);
  gtext_allocator_free(&alloc, tb->dyn_tab);
  gtext_allocator_free(&alloc, tb->merge_index[0].slots);
  gtext_allocator_free(&alloc, tb->merge_index[1].slots);
  gtext_allocator_free(&alloc, tb->stack);
  gtext_allocator_free(&alloc, tb->tmpl);
  gtext_allocator_free(&alloc, tb->pend);
  gtext_allocator_free(&alloc, tb->pend_segs);
  for (size_t i = 0; i < tb->fmt_nblocks; i++) {
    gtext_allocator_free(&alloc, tb->fmt_blocks[i]);
  }
  gtext_allocator_free(&alloc, tb->fmt_blocks);
  gtext_allocator_free(&alloc, tb->errors);
  gtext_allocator_free(&alloc, tb);
}

GTEXT_HTML_Tree * gtext_html_tree_new(const GTEXT_HTML_TreeOptions * options) {
  GTEXT_HTML_TreeOptions o;
  memset(&o, 0, sizeof(o));
  if (options) {
    o = *options;
  }
  const GTEXT_Allocator * a = o.allocator ? o.allocator : gtext_allocator_default();
  GTEXT_HTML_Tree * tb = gtext_allocator_calloc(a, 1, sizeof(*tb));
  if (!tb) {
    return NULL;
  }
  tb->alloc = *a;
  tb->opt = o;
  tb->opt.allocator = &tb->alloc;
  tb->max_open = o.max_open_elements ? o.max_open_elements
                                     : GTEXT_HTML_TREE_DEFAULT_MAX_OPEN_ELEMENTS;
  if (tb->max_open > (size_t)UINT32_MAX - 2) {
    tb->max_open = (size_t)UINT32_MAX - 2;
  }
  tb->max_errors = o.max_errors ? o.max_errors : GTEXT_HTML_TOKENIZER_DEFAULT_MAX_ERRORS;
  if (o.count_errors_only) {
    tb->max_errors = 0;
  }
  tb->seed = o.hash_seed ? o.hash_seed
                         : ((uint64_t)(uintptr_t)tb * 0x9E3779B97F4A7C15ULL) | 1;
  tb->mode = M_INITIAL;
  tb->reprocess_limit = GTEXT_HTML_TREE_REPROCESS_LIMIT;
  tb->frameset_ok = true;

  GTEXT_Markup_Options mo = gtext_markup_options_default();
  mo.allocator = &tb->alloc;
  mo.max_depth = 0;
  mo.max_attributes = 0;
  mo.max_nodes = o.max_nodes ? o.max_nodes : GTEXT_HTML_TREE_DEFAULT_MAX_NODES;
  if (gtext_markup_document_new(&mo, &tb->doc) != GTEXT_MARKUP_OK) {
    tb->doc = NULL;
    gtext_html_tree_free(tb);
    return NULL;
  }
  tb->ns_html = markup_atom_intern(
      tb->doc, GTEXT_MARKUP_NS_HTML, strlen(GTEXT_MARKUP_NS_HTML));

  tb->cap_names = T__COUNT + 32;
  tb->names = gtext_allocator_calloc(&tb->alloc, tb->cap_names, sizeof(name_slot));
  if (!tb->ns_html || !tb->names) {
    gtext_html_tree_free(tb);
    return NULL;
  }
  for (size_t i = 0; i < T__COUNT; i++) {
    name_slot * s = &tb->names[i];
    s->text = gtext_html_tag_table[i].name;
    s->len = (uint32_t)strlen(s->text);
    s->flags = gtext_html_tag_table[i].flags;
    s->topidx = NONE_IDX;
  }
  tb->nnames = T__COUNT;

  GTEXT_HTML_TokenizerOptions to;
  memset(&to, 0, sizeof(to));
  to.allocator = &tb->alloc;
  to.max_input_bytes = o.max_input_bytes;
  to.max_name_bytes = o.max_name_bytes;
  to.max_value_bytes = o.max_value_bytes;
  to.max_comment_bytes = o.max_comment_bytes;
  to.max_text_bytes = o.max_text_bytes;
  to.max_attributes = o.max_attributes;
  to.max_errors = tb->max_errors;
  to.count_errors_only = o.count_errors_only;
  to.hash_seed = o.hash_seed;
  to.allow_cdata = ht_allow_cdata;
  to.allow_cdata_user = tb;
  tb->tok = gtext_html_tokenizer_new(&to);
  if (!tb->tok) {
    gtext_html_tree_free(tb);
    return NULL;
  }
  return tb;
}

static GTEXT_HTML_TreeStatus status_from_tokenizer(GTEXT_HTML_TokenizerStatus s) {
  switch (s) {
    case GTEXT_HTML_TOKENIZER_E_OOM:
      return GTEXT_HTML_TREE_E_OOM;
    case GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT:
      return GTEXT_HTML_TREE_E_LIMIT_INPUT;
    case GTEXT_HTML_TOKENIZER_E_LIMIT_NAME:
      return GTEXT_HTML_TREE_E_LIMIT_NAME;
    case GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE:
      return GTEXT_HTML_TREE_E_LIMIT_VALUE;
    case GTEXT_HTML_TOKENIZER_E_LIMIT_COMMENT:
      return GTEXT_HTML_TREE_E_LIMIT_COMMENT;
    case GTEXT_HTML_TOKENIZER_E_LIMIT_TEXT:
      return GTEXT_HTML_TREE_E_LIMIT_TEXT;
    case GTEXT_HTML_TOKENIZER_E_LIMIT_ATTRIBUTES:
      return GTEXT_HTML_TREE_E_LIMIT_ATTRIBUTES;
    case GTEXT_HTML_TOKENIZER_E_STATE:
      return GTEXT_HTML_TREE_E_STATE;
    default:
      return GTEXT_HTML_TREE_E_INVALID;
  }
}

/* Take every token the tokenizer has, until it wants input or is done. */
static void drain(GTEXT_HTML_Tree * tb) {
  for (;;) {
    const GTEXT_HTML_Token * t = NULL;
    const GTEXT_HTML_TokenizerStatus s = gtext_html_tokenizer_next(tb->tok, &t);
    sync_tokenizer_errors(tb);
    if (tb->status != GTEXT_HTML_TREE_OK) {
      return;
    }
    if (s == GTEXT_HTML_TOKENIZER_TOKEN) {
      if (!ht_tree_handle_token(tb, t)) {
        return;
      }
      continue;
    }
    if (s == GTEXT_HTML_TOKENIZER_NEED_INPUT) {
      return;
    }
    if (s == GTEXT_HTML_TOKENIZER_DONE) {
      tb->tok_done = true;
      return;
    }
    ht_tree_fail(tb, status_from_tokenizer(s));
    return;
  }
}

GTEXT_HTML_TreeStatus gtext_html_tree_feed(
    GTEXT_HTML_Tree * tb, const void * data, size_t length) {
  if (!tb || (!data && length != 0)) {
    return GTEXT_HTML_TREE_E_INVALID;
  }
  if (tb->status != GTEXT_HTML_TREE_OK) {
    return tb->status;
  }
  if (tb->finished) {
    return GTEXT_HTML_TREE_E_STATE;
  }
  const GTEXT_HTML_TokenizerStatus s = gtext_html_tokenizer_feed(tb->tok, data, length);
  if (s != GTEXT_HTML_TOKENIZER_OK) {
    ht_tree_fail(tb, status_from_tokenizer(s));
    return tb->status;
  }
  drain(tb);
  return tb->status;
}

GTEXT_HTML_TreeStatus gtext_html_tree_finish(GTEXT_HTML_Tree * tb) {
  if (!tb) {
    return GTEXT_HTML_TREE_E_INVALID;
  }
  if (tb->status != GTEXT_HTML_TREE_OK || tb->finished) {
    return tb->status;
  }
  gtext_html_tokenizer_finish(tb->tok);
  drain(tb);
  if (tb->status == GTEXT_HTML_TREE_OK && !(tb->tok_done && tb->stopped)) {
    /* The tokenizer must have delivered its end-of-file token and the builder must have handled it. */
    ht_tree_fail(tb, GTEXT_HTML_TREE_E_INVALID);
  }
  if (tb->status == GTEXT_HTML_TREE_OK) {
    tb->finished = true;
    release_atoms(tb);
  }
  return tb->status;
}

GTEXT_HTML_TreeStatus gtext_html_tree_skip_input(GTEXT_HTML_Tree * tb, size_t count) {
  if (!tb) {
    return GTEXT_HTML_TREE_E_INVALID;
  }
  if (tb->status != GTEXT_HTML_TREE_OK) {
    return tb->status;
  }
  const GTEXT_HTML_TokenizerStatus s = gtext_html_tokenizer_skip_input(tb->tok, count);
  if (s == GTEXT_HTML_TOKENIZER_E_STATE) {
    return GTEXT_HTML_TREE_E_STATE;
  }
  if (s != GTEXT_HTML_TOKENIZER_OK) {
    ht_tree_fail(tb, status_from_tokenizer(s));
  }
  return tb->status;
}

GTEXT_Markup_Document * gtext_html_tree_take_document(GTEXT_HTML_Tree * tb) {
  if (!tb || !tb->finished || tb->status != GTEXT_HTML_TREE_OK || tb->doc_taken || tb->frag) {
    return NULL; /* a fragment builder hands over its document with the fragment */
  }
  tb->doc_taken = true;
  GTEXT_Markup_Document * doc = tb->doc;
  tb->doc = NULL;
  return doc;
}

GTEXT_HTML_TreeStatus gtext_html_tree_status(const GTEXT_HTML_Tree * tb) {
  return tb ? tb->status : GTEXT_HTML_TREE_E_INVALID;
}

GTEXT_HTML_TreeStatus gtext_html_tree_parse(const void * data, size_t length,
    const GTEXT_HTML_TreeOptions * options, GTEXT_Markup_Document ** out,
    GTEXT_HTML_Tree ** tree_out) {
  if (tree_out) {
    *tree_out = NULL;
  }
  if (!out || (!data && length != 0)) {
    return GTEXT_HTML_TREE_E_INVALID;
  }
  GTEXT_HTML_Tree * tb = gtext_html_tree_new(options);
  if (tree_out) {
    *tree_out = tb;
  }
  if (!tb) {
    return GTEXT_HTML_TREE_E_OOM;
  }
  GTEXT_HTML_TreeStatus s = gtext_html_tree_feed(tb, data, length);
  if (s == GTEXT_HTML_TREE_OK) {
    s = gtext_html_tree_finish(tb);
  }
  if (s == GTEXT_HTML_TREE_OK) {
    *out = gtext_html_tree_take_document(tb);
  }
  if (!tree_out) {
    gtext_html_tree_free(tb);
  }
  return s;
}

size_t gtext_html_tree_error_count(const GTEXT_HTML_Tree * tb) {
  return tb ? tb->nerrors : 0;
}

const GTEXT_HTML_TreeError * gtext_html_tree_error_at(
    const GTEXT_HTML_Tree * tb, size_t index) {
  return tb && index < tb->nerrors ? &tb->errors[index] : NULL;
}

size_t gtext_html_tree_errors_dropped(const GTEXT_HTML_Tree * tb) {
  return tb ? tb->dropped + gtext_html_tokenizer_errors_dropped(tb->tok) : 0;
}

size_t gtext_html_tree_work(const GTEXT_HTML_Tree * tb) {
  return tb ? tb->work : 0;
}

size_t gtext_html_tree_tokenizer_work(const GTEXT_HTML_Tree * tb) {
  return tb ? gtext_html_tokenizer_work(tb->tok) : 0;
}

void gtext_html_tree_test_set_reprocess_limit(GTEXT_HTML_Tree * tb, size_t n) {
  if (tb) {
    tb->reprocess_limit = n;
  }
}

const char * gtext_html_tree_mode_name(const GTEXT_HTML_Tree * tb) {
  return tb ? ht_tree_mode_name(tb->mode) : "";
}

/* ---- names of things ---------------------------------------------------- */

static const char * const k_tree_error_names[GTEXT_HTML_TREE_ERR_COUNT] = {
    "missing-doctype",
    "non-conforming-doctype",
    "unexpected-doctype",
    "unexpected-start-tag",
    "unexpected-end-tag",
    "unexpected-character",
    "unexpected-null-character",
    "unexpected-eof",
    "open-elements-at-eof",
    "open-elements-at-end-tag",
    "misnested-formatting",
    "nested-form",
    "unexpected-element-in-context",
    "deprecated-tag",
    "html-in-foreign-content",
    "xmlns-attribute-mismatch",
};

const char * gtext_html_tree_error_name(int code) {
  if (code >= 0 && code < (int)GTEXT_HTML_ERR_COUNT) {
    return gtext_html_error_code_name((GTEXT_HTML_ErrorCode)code);
  }
  if (code >= GTEXT_HTML_TREE_ERROR_BASE
      && code < GTEXT_HTML_TREE_ERROR_BASE + (int)GTEXT_HTML_TREE_ERR_COUNT) {
    return k_tree_error_names[code - GTEXT_HTML_TREE_ERROR_BASE];
  }
  return "unknown";
}
