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
 * The Markup binding (css_cssom_markup.h): `<style>` and `<link rel=stylesheet>` owner sheets, `sync` with the generation shortcut, the removed-owner lifecycle, and the attribute binding of
 * the `style` attribute.
 *
 * **The record.** The binding keeps one @ref mentry for each element that has, or is waiting for, a sheet, in the tree order of the last sync (`order`), with a hash table over the element
 * pointer (`map`, used only to find an entry; no order or result depends on an address). An entry holds the text the sheet was made from (a `<style>`) or a fingerprint of the attributes
 * that decide a `<link>`'s, the remembered `media` and `title`, and the sheet, with one external reference of the binding's. So a sheet the caller released, or the document removed from the
 * scope, is still the object the binding means.
 *
 * **A sync is a diff.** One walk collects the elements that qualify, in tree order. The entries that are still elements of the same kind and keep their relative order (the longest
 * increasing run of their old positions) stay; every other entry is a removed or moved element and loses its sheet; then one pass over the elements keeps what is unchanged and makes what is
 * new. A step that fails stops the pass with the entries describing exactly what has been done, and the state that tells what is still to do (an entry that needs its sheet): the next call
 * goes on.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_cssom.h>
#include <ghoti.io/text/css/css_cssom_markup.h>

#include "cssom_doc_internal.h"
#include "cssom_internal.h"
#include "cssom_rule_internal.h"

#define XHTML_NS GTEXT_MARKUP_NS_HTML
#define SVG_NS GTEXT_MARKUP_NS_SVG

typedef enum {
  ENTRY_SHEET = 0,     ///< It has its sheet.
  ENTRY_NEEDS,         ///< It should have one and does not (a step failed): the next call makes it.
  ENTRY_PENDING,       ///< A `<link>` waiting for gtext_cssom_markup_link_complete().
  ENTRY_FAILED         ///< A `<link>` the callback said has no sheet; asked again only when its attributes change.
} entry_state;

typedef struct mentry {
  const GTEXT_Markup_Node * element;
  GTEXT_CSSOM_Sheet * sheet;       ///< One external reference, or NULL.
  entry_state state;
  bool link;
  char * text;                     ///< A style: the text the sheet was made from. A link: the fingerprint of the attributes that decided it.
  size_t text_length;
  char * media, * title;           ///< The attributes as they were; NULL for absent.
  size_t media_length, title_length;
  size_t children;                 ///< A style: the number of Text and CDATA children, empty ones too (the HTML Standard's "children changed steps" reparse for each change of them).
  size_t old_index;                ///< Scratch of a sync.
  bool stay;
} mentry;

typedef struct {
  const GTEXT_Markup_Node * element;
  bool link;
} cand;

struct GTEXT_CSSOM_Markup_Binding {
  GTEXT_CSSOM_Scope * scope;       ///< Retained.
  const GTEXT_Markup_Document * markup;
  GTEXT_Allocator alloc;
  GTEXT_CSSOM_Markup_Options options;
  char * base_url;                 ///< A copy.
  char * preferred;                ///< CSSOM's preferred CSS style sheet set name ("" until a sheet with a title and no alternate flag is added); NULL is the empty string.
  size_t preferred_length;
  mentry ** order;
  size_t n, cap;
  mentry ** map;                   ///< Open addressing; NULL is empty, TOMB a deleted slot.
  size_t mcap, mused, mlive;
  uint64_t last_generation;
  bool synced;
  char * scratch;
  size_t scratch_cap;
  GTEXT_CSSOM_Markup_Counters counters;
};

static mentry tomb_entry;
#define TOMB (&tomb_entry)

/* ---- small things --------------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status status_of(GTEXT_Markup_Status st) {
  switch (st) {
  case GTEXT_MARKUP_OK: return GTEXT_CSS_OK;
  case GTEXT_MARKUP_E_OOM: return GTEXT_CSS_E_OOM;
  case GTEXT_MARKUP_E_LIMIT: return GTEXT_CSS_E_LIMIT_CSSOM;
  default: return GTEXT_CSS_E_INVALID;
  }
}

static char * dup_n(GTEXT_CSSOM_Markup_Binding * b, const char * s, size_t n) {
  char * d = gtext_allocator_malloc(&b->alloc, n + 1);
  if (d) {
    if (n) {
      memcpy(d, s, n);
    }
    d[n] = '\0';
  }
  return d;
}

static void entry_free(GTEXT_CSSOM_Markup_Binding * b, mentry * e) {
  if (!e) {
    return;
  }
  gtext_cssom_sheet_release(e->sheet);
  gtext_allocator_free(&b->alloc, e->text);
  gtext_allocator_free(&b->alloc, e->media);
  gtext_allocator_free(&b->alloc, e->title);
  gtext_allocator_free(&b->alloc, e);
}

static size_t hash_of(const void * p) {
  uintptr_t x = (uintptr_t)p >> 3;
  x ^= x >> 17;
  x *= (uintptr_t)0x9e3779b97f4a7c15ull;
  x ^= x >> 29;
  return (size_t)x;
}

static mentry * map_find(const GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * el) {
  if (!b->mcap) {
    return NULL;
  }
  size_t i = hash_of(el) & (b->mcap - 1);
  for (size_t probes = 0; probes < b->mcap; probes++, i = (i + 1) & (b->mcap - 1)) {
    mentry * e = b->map[i];
    if (!e) {
      return NULL;
    }
    if (e != TOMB && e->element == el) {
      return e;
    }
  }
  return NULL;
}

static bool map_rebuild(GTEXT_CSSOM_Markup_Binding * b, size_t want) {
  size_t cap = 16;
  while (cap < want * 2) {
    cap *= 2;
  }
  mentry ** slots = gtext_allocator_calloc(&b->alloc, cap, sizeof *slots);
  if (!slots) {
    return false;
  }
  for (size_t k = 0; k < b->mcap; k++) {
    mentry * e = b->map[k];
    if (e && e != TOMB) {
      size_t i = hash_of(e->element) & (cap - 1);
      while (slots[i]) {
        i = (i + 1) & (cap - 1);
      }
      slots[i] = e;
    }
  }
  gtext_allocator_free(&b->alloc, b->map);
  b->map = slots;
  b->mcap = cap;
  b->mused = b->mlive;
  return true;
}

static bool map_add(GTEXT_CSSOM_Markup_Binding * b, mentry * e) {
  if ((b->mused + 1) * 2 > b->mcap && !map_rebuild(b, b->mlive + 1)) {
    return false;
  }
  size_t i = hash_of(e->element) & (b->mcap - 1);
  while (b->map[i] && b->map[i] != TOMB) {
    i = (i + 1) & (b->mcap - 1);
  }
  if (!b->map[i]) {
    b->mused++;
  }
  b->map[i] = e;
  b->mlive++;
  return true;
}

static void map_del(GTEXT_CSSOM_Markup_Binding * b, mentry * e) {
  size_t i = hash_of(e->element) & (b->mcap - 1);
  for (size_t probes = 0; probes < b->mcap; probes++, i = (i + 1) & (b->mcap - 1)) {
    if (b->map[i] == e) {
      b->map[i] = TOMB;
      b->mlive--;
      return;
    }
    if (!b->map[i]) {
      return;
    }
  }
}

static bool order_reserve(GTEXT_CSSOM_Markup_Binding * b, size_t extra) {
  if (b->n + extra <= b->cap) {
    return true;
  }
  size_t cap = b->cap ? b->cap : 8;
  while (cap < b->n + extra) {
    cap *= 2;
  }
  mentry ** grown = gtext_allocator_realloc(&b->alloc, b->order, cap * sizeof *grown);
  if (!grown) {
    return false;
  }
  b->order = grown;
  b->cap = cap;
  return true;
}

static bool scratch_reserve(GTEXT_CSSOM_Markup_Binding * b, size_t n) {
  if (n <= b->scratch_cap) {
    return true;
  }
  size_t cap = b->scratch_cap ? b->scratch_cap : 256;
  while (cap < n) {
    cap *= 2;
  }
  char * grown = gtext_allocator_realloc(&b->alloc, b->scratch, cap);
  if (!grown) {
    return false;
  }
  b->scratch = grown;
  b->scratch_cap = cap;
  return true;
}

/* ---- reading an element ---------------------------------------------------------------------------------------------------------------------------- */

static bool attr(const GTEXT_Markup_Node * el, const char * name, const char ** v, size_t * n) {
  GTEXT_Markup_Attribute a;
  if (!gtext_markup_element_get_attribute(el, NULL, name, &a)) {
    *v = NULL;
    *n = 0;
    return false;
  }
  *v = a.value;
  *n = a.value_length;
  return true;
}

static bool ci_equal(const char * s, size_t n, const char * lower) {
  if (strlen(lower) != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + 32);
    }
    if (c != lower[i]) {
      return false;
    }
  }
  return true;
}

static bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/// A `type` attribute that allows a style sheet: absent, empty or, whole, an ASCII case-insensitive `text/css` (the browsers do not trim it, nor accept parameters).
static bool type_ok(const GTEXT_Markup_Node * el) {
  const char * v;
  size_t n;
  if (!attr(el, "type", &v, &n)) {
    return true;
  }
  return n == 0 || ci_equal(v, n, "text/css");
}

/// Whether `rel` has the keyword (ASCII case-insensitive).
static bool rel_has(const char * rel, size_t n, const char * keyword) {
  size_t i = 0;
  while (i < n) {
    while (i < n && is_space(rel[i])) {
      i++;
    }
    size_t j = i;
    while (j < n && !is_space(rel[j])) {
      j++;
    }
    if (j > i && ci_equal(rel + i, j - i, keyword)) {
      return true;
    }
    i = j;
  }
  return false;
}

static bool is_style(const GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * el) {
  return (b->options.html && gtext_markup_element_is(el, XHTML_NS, "style")) || (b->options.svg && gtext_markup_element_is(el, SVG_NS, "style"));
}

static bool is_link(const GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * el) {
  return b->options.html && gtext_markup_element_is(el, XHTML_NS, "link");
}

/// Whether the element is a style or link element that has a sheet in the HTML Standard's terms (connected is the caller's business).
static bool qualifies(const GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * el, bool * link) {
  if (gtext_markup_node_kind(el) != GTEXT_MARKUP_KIND_ELEMENT) {
    return false;
  }
  if (is_style(b, el)) {
    *link = false;
    return type_ok(el);
  }
  if (is_link(b, el)) {
    const char * rel;
    size_t n;
    if (!attr(el, "rel", &rel, &n) || !rel_has(rel, n, "stylesheet") || !type_ok(el)) {
      return false;
    }
    const char * href;
    size_t hn;
    if (!attr(el, "href", &href, &hn) || hn == 0) {
      return false;
    }
    *link = true;
    return true;
  }
  return false;
}

/// The child text content of a style element: its Text and CDATA children in order. One child is read in place; several are put in the scratch buffer.
static GTEXT_CSS_Status style_text(GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * el, const char ** text, size_t * length) {
  size_t total = 0;
  const GTEXT_Markup_Node * only = NULL;
  size_t count = 0;
  for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(el); c; c = gtext_markup_node_next_sibling(c)) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
    if (k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA) {
      size_t n = 0;
      gtext_markup_node_data(c, &n);
      if (n) {
        total += n;
        count++;
        only = c;
      }
    }
  }
  if (count <= 1) {
    if (count == 1) {
      *text = gtext_markup_node_data(only, length);
    }
    else {
      *text = "";
      *length = 0;
    }
    return GTEXT_CSS_OK;
  }
  if (!scratch_reserve(b, total + 1)) {
    return GTEXT_CSS_E_OOM;
  }
  size_t at = 0;
  for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(el); c; c = gtext_markup_node_next_sibling(c)) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
    if (k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA) {
      size_t n = 0;
      const char * d = gtext_markup_node_data(c, &n);
      if (n) {
        memcpy(b->scratch + at, d, n);
        at += n;
      }
    }
  }
  b->scratch[at] = '\0';
  *text = b->scratch;
  *length = at;
  return GTEXT_CSS_OK;
}

/// The Text and CDATA children of a style element, empty ones included: a node added or removed is a change of the children, which reparses the sheet (HTML Standard, "update a style
/// block") even when the text stays what it was.
static size_t style_children(const GTEXT_Markup_Node * el) {
  size_t n = 0;
  for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(el); c; c = gtext_markup_node_next_sibling(c)) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
    if (k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA) {
      n++;
    }
  }
  return n;
}

/// The fingerprint of the attributes that decide a link's sheet: rel, href, type, media, title and disabled, each followed by a NUL, in the scratch buffer.
static GTEXT_CSS_Status link_fingerprint(GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * el, const char ** text, size_t * length) {
  static const char * const names[] = {"rel", "href", "type", "media", "title", "disabled"};
  const char * vals[6];
  size_t lens[6];
  size_t total = 0;
  for (int i = 0; i < 6; i++) {
    const bool present = attr(el, names[i], &vals[i], &lens[i]);
    total += lens[i] + 2;   /* the value, a presence mark, a NUL */
    (void)present;
  }
  if (!scratch_reserve(b, total + 1)) {
    return GTEXT_CSS_E_OOM;
  }
  size_t at = 0;
  for (int i = 0; i < 6; i++) {
    const char * v;
    size_t n;
    const bool present = attr(el, names[i], &v, &n);
    b->scratch[at++] = present ? '+' : '-';
    if (n) {
      memcpy(b->scratch + at, v, n);
      at += n;
    }
    b->scratch[at++] = '\0';
  }
  *text = b->scratch;
  *length = at;
  return GTEXT_CSS_OK;
}

/* ---- the scope's list ------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSSOM_Document * document_of(const GTEXT_CSSOM_Markup_Binding * b) {
  return gtext_cssom_scope_document(b->scope);
}

/// Where a sheet is in the scope's `styleSheets`, looking at `hint` first.
static bool locate(const GTEXT_CSSOM_Markup_Binding * b, const GTEXT_CSSOM_Sheet * sheet, size_t hint, size_t * index) {
  const css_scope * s = (const css_scope *)(const void *)b->scope;
  if (hint < s->nsheets && (const void *)s->sheets[hint] == (const void *)sheet) {
    *index = hint;
    return true;
  }
  for (size_t i = 0; i < s->nsheets; i++) {
    if ((const void *)s->sheets[i] == (const void *)sheet) {
      *index = i;
      return true;
    }
  }
  return false;
}

/// The index a new sheet goes to: after the sheet of the nearest entry before position `at` of `order` that has one, else the start. `hint` is one past where that sheet is expected (0: no idea).
static size_t insertion_index(const GTEXT_CSSOM_Markup_Binding * b, size_t at, size_t hint) {
  for (size_t i = at; i > 0; i--) {
    const mentry * p = b->order[i - 1];
    if (p->sheet) {
      size_t index = 0;
      if (locate(b, p->sheet, hint ? hint - 1 : (size_t)-1, &index)) {
        return index + 1;
      }
      break;
    }
  }
  return 0;
}

static GTEXT_CSS_Status take_out(GTEXT_CSSOM_Markup_Binding * b, mentry * e, GTEXT_CSSOM_Outcome * outcome) {
  if (!e->sheet) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSSOM_Outcome o;
  memset(&o, 0, sizeof o);
  const GTEXT_CSS_Status st = gtext_cssom_scope_remove_sheet(b->scope, e->sheet, &o);
  if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_NOT_FOUND) {
    return st;   /* NOT_FOUND: the caller took it out already; it is out */
  }
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
    if (o.observer_status != GTEXT_CSS_OK && outcome->observer_status == GTEXT_CSS_OK) {
      outcome->observer_status = o.observer_status;
    }
  }
  gtext_cssom_sheet_release(e->sheet);
  e->sheet = NULL;
  e->state = ENTRY_NEEDS;
  b->counters.sheets_removed++;
  return GTEXT_CSS_OK;
}

/// Make the element's sheet and list it at `index`. The entry has `text` remembered already.
static GTEXT_CSS_Status put_sheet(GTEXT_CSSOM_Markup_Binding * b, mentry * e, const char * text, size_t length, const GTEXT_CSSOM_Link_Text * link, size_t index, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Document * doc = document_of(b);
  if (!doc) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSSOM_Sheet_Options so = gtext_cssom_sheet_options_default();
  so.owner_node = e->element;
  so.implicit_root = gtext_markup_node_parent(e->element);
  so.title = e->title;
  so.media = e->media;
  so.base_url = link && link->base_url ? link->base_url : b->base_url;
  if (link) {
    so.origin_clean = link->origin_clean;
    const char * href;
    size_t hn;
    attr(e->element, "href", &href, &hn);
    so.href = link->href ? link->href : href;   /* a Markup attribute value is NUL-terminated */
    const char * rel;
    size_t rn;
    if (attr(e->element, "rel", &rel, &rn)) {
      so.alternate = rel_has(rel, rn, "alternate");
    }
    const char * d;
    size_t dn;
    so.disabled = attr(e->element, "disabled", &d, &dn) || so.alternate;
  }
  /* CSSOM, "add a CSS style sheet": "If the title is not the empty string, the alternate flag is unset, and preferred CSS style sheet set name is the empty string change the
     preferred CSS style sheet set name to the title", and the disabled flag is unset when "The title is the empty string" or "The last CSS style sheet set name is null and the title
     is a case-sensitive match for the preferred CSS style sheet set name", and set otherwise (a sheet that was disabled to begin with stays so). The last name is never selected here. */
  const bool titled = e->title && e->title_length > 0;
  bool set_preferred = false;
  if (!so.disabled) {
    if (titled && !so.alternate && b->preferred_length == 0) {
      char * name = dup_n(b, e->title, e->title_length);
      if (!name) {
        return GTEXT_CSS_E_OOM;
      }
      gtext_allocator_free(&b->alloc, b->preferred);
      b->preferred = name;
      b->preferred_length = e->title_length;
      set_preferred = true;
    }
    so.disabled = titled && !(b->preferred_length == e->title_length && memcmp(b->preferred, e->title, e->title_length) == 0);
  }
  GTEXT_CSSOM_Sheet * sheet = NULL;
  GTEXT_CSS_Status st = gtext_cssom_sheet_new_parsed(doc, text, length, &so, &sheet);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Outcome o;
  memset(&o, 0, sizeof o);
  st = gtext_cssom_scope_insert_sheet(b->scope, sheet, index, &o);
  if (st != GTEXT_CSS_OK) {
    gtext_cssom_sheet_release(sheet);
    return st;
  }
  e->sheet = sheet;
  e->state = ENTRY_SHEET;
  b->counters.sheets_made++;
  if (set_preferred) {
    /* "enable a CSS style sheet set" with the new name: the sheets of other sets are disabled, those of this one are not */
    for (size_t i = 0; i < b->n && st == GTEXT_CSS_OK; i++) {
      mentry * o = b->order[i];
      if (o == e || !o->sheet || !o->title || o->title_length == 0) {
        continue;
      }
      const bool same = o->title_length == b->preferred_length && memcmp(o->title, b->preferred, o->title_length) == 0;
      st = gtext_cssom_sheet_set_disabled(o->sheet, !same, NULL);
    }
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
    if (o.observer_status != GTEXT_CSS_OK && outcome->observer_status == GTEXT_CSS_OK) {
      outcome->observer_status = o.observer_status;
    }
  }
  return GTEXT_CSS_OK;
}

/// Read the element for its entry and (re)make its sheet at the place the entries say. `at` is the entry's position in `order`; `hint` is where the previous sheet should be.
static GTEXT_CSS_Status build_entry(GTEXT_CSSOM_Markup_Binding * b, mentry * e, size_t at, size_t hint, GTEXT_CSSOM_Outcome * outcome) {
  const char * v;
  size_t n;
  /* the attributes the sheet is made with */
  char * media = NULL, * title = NULL;
  size_t media_length = 0, title_length = 0;
  const bool has_media = attr(e->element, "media", &v, &n);
  if (has_media && !(media = dup_n(b, v, n))) {
    return GTEXT_CSS_E_OOM;
  }
  media_length = n;
  const bool has_title = attr(e->element, "title", &v, &n);
  if (has_title && !(title = dup_n(b, v, n))) {
    gtext_allocator_free(&b->alloc, media);
    return GTEXT_CSS_E_OOM;
  }
  title_length = n;
  const char * text = NULL;
  size_t length = 0;
  GTEXT_CSS_Status st = e->link ? link_fingerprint(b, e->element, &text, &length) : style_text(b, e->element, &text, &length);
  char * remembered = NULL;
  if (st == GTEXT_CSS_OK && !(remembered = dup_n(b, text, length))) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&b->alloc, media);
    gtext_allocator_free(&b->alloc, title);
    return st;
  }
  /* the entry now describes the element as it is read */
  gtext_allocator_free(&b->alloc, e->text);
  e->text = remembered;
  e->text_length = length;
  e->children = e->link ? 0 : style_children(e->element);
  gtext_allocator_free(&b->alloc, e->media);
  e->media = media;
  e->media_length = media_length;
  gtext_allocator_free(&b->alloc, e->title);
  e->title = title;
  e->title_length = title_length;
  if (!e->link) {
    e->state = ENTRY_NEEDS;
    return put_sheet(b, e, text, length, NULL, insertion_index(b, at, hint), outcome);
  }
  /* a link: ask */
  const char * rel, * href;
  size_t rn, hn;
  attr(e->element, "rel", &rel, &rn);
  attr(e->element, "href", &href, &hn);
  if (!b->options.link) {
    e->state = ENTRY_FAILED;
    return GTEXT_CSS_OK;
  }
  char * hrefz = dup_n(b, href, hn);
  char * relz = dup_n(b, rel, rn);
  char * mediaz = dup_n(b, e->media ? e->media : "", e->media_length);
  char * titlez = dup_n(b, e->title ? e->title : "", e->title_length);
  if (!hrefz || !relz || !mediaz || !titlez) {
    gtext_allocator_free(&b->alloc, hrefz);
    gtext_allocator_free(&b->alloc, relz);
    gtext_allocator_free(&b->alloc, mediaz);
    gtext_allocator_free(&b->alloc, titlez);
    e->state = ENTRY_NEEDS;
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSSOM_Link_Request req;
  memset(&req, 0, sizeof req);
  req.size = sizeof req;
  req.element = e->element;
  req.href = hrefz;
  req.href_length = hn;
  req.rel = relz;
  req.media = mediaz;
  req.title = titlez;
  req.alternate = rel_has(rel, rn, "alternate");
  const char * d;
  size_t dn;
  req.disabled = attr(e->element, "disabled", &d, &dn);
  GTEXT_CSSOM_Link_Text lt;
  memset(&lt, 0, sizeof lt);
  lt.size = sizeof lt;
  lt.origin_clean = true;
  GTEXT_CSSOM_Link_Outcome lo = GTEXT_CSSOM_LINK_FAILED;
  b->counters.link_requests++;
  st = b->options.link(b->options.link_user, &req, &lt, &lo);
  gtext_allocator_free(&b->alloc, hrefz);
  gtext_allocator_free(&b->alloc, relz);
  gtext_allocator_free(&b->alloc, mediaz);
  gtext_allocator_free(&b->alloc, titlez);
  if (st != GTEXT_CSS_OK) {
    e->state = ENTRY_NEEDS;
    return st;
  }
  if (lo == GTEXT_CSSOM_LINK_PENDING) {
    e->state = ENTRY_PENDING;
    return GTEXT_CSS_OK;
  }
  if (lo == GTEXT_CSSOM_LINK_FAILED) {
    e->state = ENTRY_FAILED;
    return GTEXT_CSS_OK;
  }
  e->state = ENTRY_NEEDS;
  return put_sheet(b, e, lt.text ? lt.text : "", lt.length, &lt, insertion_index(b, at, hint), outcome);
}

/// An entry that has its sheet and whose element may have changed: keep it, change it in place, or replace it.
static GTEXT_CSS_Status refresh_entry(GTEXT_CSSOM_Markup_Binding * b, mentry * e, size_t at, size_t hint, GTEXT_CSSOM_Outcome * outcome) {
  if (e->sheet && gtext_cssom_sheet_owner_node(e->sheet) != e->element) {
    /* the caller took the sheet out of the list: the element has none now */
    gtext_cssom_sheet_release(e->sheet);
    e->sheet = NULL;
    e->state = ENTRY_NEEDS;
  }
  if (e->state == ENTRY_NEEDS) {
    return build_entry(b, e, at, hint, outcome);
  }
  if (e->sheet && ((const css_sheet *)(const void *)e->sheet)->implicit_root != (const void *)gtext_markup_node_parent(e->element)) {
    /* the element was moved under another parent: the HTML Standard removes the sheet of a removed style element and makes one on insertion, and the implicit scope root
       of an `@scope` with no prelude is the parent the sheet was made under (the Cascade page scope-implicit-sibling-mutations.html moves a style between siblings) */
    GTEXT_CSS_Status moved = take_out(b, e, outcome);
    if (moved != GTEXT_CSS_OK) {
      return moved;
    }
    return build_entry(b, e, at, hint, outcome);
  }
  const char * text = NULL;
  size_t length = 0;
  GTEXT_CSS_Status st = e->link ? link_fingerprint(b, e->element, &text, &length) : style_text(b, e->element, &text, &length);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!e->link) {
    b->counters.texts_compared++;
    b->counters.bytes_compared += length < e->text_length ? length : e->text_length;
  }
  if (!e->link && style_children(e->element) != e->children) {
    /* the children changed, an empty text node included, whatever the text says: "update a style block" */
    st = take_out(b, e, outcome);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    return build_entry(b, e, at, hint, outcome);
  }
  if (length != e->text_length || (length && memcmp(text, e->text, length) != 0)) {
    /* the HTML Standard's "update a style block": the old sheet goes and a new one is made (a pending or failed link is asked again) */
    st = take_out(b, e, outcome);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    return build_entry(b, e, at, hint, outcome);
  }
  if (e->state != ENTRY_SHEET) {
    return GTEXT_CSS_OK;   /* a pending or failed link whose attributes are the same */
  }
  /* the text is the same: the media and the title are changed in place */
  const char * v;
  size_t n;
  const bool has_media = attr(e->element, "media", &v, &n);
  if (has_media != (e->media != NULL) || (has_media && (n != e->media_length || memcmp(v, e->media, n) != 0))) {
    char * copy = has_media ? dup_n(b, v, n) : NULL;
    if (has_media && !copy) {
      return GTEXT_CSS_E_OOM;
    }
    GTEXT_CSSOM_Media_List * list = NULL;
    st = gtext_cssom_sheet_media(e->sheet, &list);
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSSOM_Outcome o;
      memset(&o, 0, sizeof o);
      st = gtext_cssom_media_set_text(list, has_media ? v : "", has_media ? n : 0, &o);
      gtext_cssom_media_release(list);
      if (st == GTEXT_CSS_OK && outcome && o.kind == GTEXT_CSSOM_CHANGED) {
        outcome->kind = GTEXT_CSSOM_CHANGED;
        if (o.observer_status != GTEXT_CSS_OK && outcome->observer_status == GTEXT_CSS_OK) {
          outcome->observer_status = o.observer_status;
        }
      }
    }
    if (st != GTEXT_CSS_OK) {
      gtext_allocator_free(&b->alloc, copy);
      return st;
    }
    gtext_allocator_free(&b->alloc, e->media);
    e->media = copy;
    e->media_length = has_media ? n : 0;
  }
  const bool has_title = attr(e->element, "title", &v, &n);
  if (has_title != (e->title != NULL) || (has_title && (n != e->title_length || memcmp(v, e->title, n) != 0))) {
    char * copy = has_title ? dup_n(b, v, n) : NULL;
    if (has_title && !copy) {
      return GTEXT_CSS_E_OOM;
    }
    if (!css_sheet_set_title((css_sheet *)(void *)e->sheet, has_title ? v : NULL, has_title ? n : 0)) {
      gtext_allocator_free(&b->alloc, copy);
      return GTEXT_CSS_E_OOM;
    }
    gtext_allocator_free(&b->alloc, e->title);
    e->title = copy;
    e->title_length = has_title ? n : 0;
  }
  return GTEXT_CSS_OK;
}

/* ---- the binding ------------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSSOM_Markup_Options gtext_cssom_markup_options_default(void) {
  GTEXT_CSSOM_Markup_Options o;
  memset(&o, 0, sizeof o);
  o.size = sizeof o;
  o.html = true;
  o.svg = true;
  return o;
}

GTEXT_CSS_Status gtext_cssom_markup_binding_new(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Scope * scope, const GTEXT_Markup_Document * markup, const GTEXT_CSSOM_Markup_Options * options,
    GTEXT_CSSOM_Markup_Binding ** out) {
  if (!document || !scope || !markup || !out || (options && options->size < offsetof(GTEXT_CSSOM_Markup_Options, base_url))) {
    return GTEXT_CSS_E_INVALID;
  }
  if (gtext_cssom_scope_document(scope) != document) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_Allocator a = *gtext_cssom_document_allocator(document);
  GTEXT_CSSOM_Markup_Binding * b = gtext_allocator_calloc(&a, 1, sizeof *b);
  if (!b) {
    return GTEXT_CSS_E_OOM;
  }
  b->alloc = a;
  b->options = gtext_cssom_markup_options_default();
  if (options) {
    const size_t n = options->size < sizeof b->options ? options->size : sizeof b->options;
    memcpy(&b->options, options, n);
    b->options.size = sizeof b->options;
    if (n < offsetof(GTEXT_CSSOM_Markup_Options, link_user) + sizeof options->link_user) {
      b->options.link = NULL;
      b->options.link_user = NULL;
    }
  }
  if (b->options.base_url) {
    b->base_url = dup_n(b, b->options.base_url, strlen(b->options.base_url));
    if (!b->base_url) {
      gtext_allocator_free(&a, b);
      return GTEXT_CSS_E_OOM;
    }
    b->options.base_url = b->base_url;
  }
  b->scope = gtext_cssom_scope_retain(scope);
  b->markup = markup;
  *out = b;
  return GTEXT_CSS_OK;
}

void gtext_cssom_markup_binding_free(GTEXT_CSSOM_Markup_Binding * b) {
  if (!b) {
    return;
  }
  GTEXT_Allocator a = b->alloc;
  for (size_t i = 0; i < b->n; i++) {
    entry_free(b, b->order[i]);
  }
  gtext_allocator_free(&a, b->order);
  gtext_allocator_free(&a, b->map);
  gtext_allocator_free(&a, b->scratch);
  gtext_allocator_free(&a, b->base_url);
  gtext_allocator_free(&a, b->preferred);
  gtext_cssom_scope_release(b->scope);
  gtext_allocator_free(&a, b);
}

/* ---- sync -------------------------------------------------------------------------------------------------------------------------------------------------- */

/// The longest strictly increasing run of `seq` (entries of -1 skipped): marks `keep[i]` for the members. `seq` is of `n` values.
static bool lis_mark(GTEXT_CSSOM_Markup_Binding * b, const int64_t * seq, size_t n, bool * keep) {
  size_t * tails = gtext_allocator_malloc(&b->alloc, (n ? n : 1) * sizeof *tails);
  size_t * prev = gtext_allocator_malloc(&b->alloc, (n ? n : 1) * sizeof *prev);
  if (!tails || !prev) {
    gtext_allocator_free(&b->alloc, tails);
    gtext_allocator_free(&b->alloc, prev);
    return false;
  }
  size_t len = 0;
  for (size_t i = 0; i < n; i++) {
    keep[i] = false;
    if (seq[i] < 0) {
      continue;
    }
    size_t lo = 0, hi = len;
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (seq[tails[mid]] < seq[i]) {
        lo = mid + 1;
      }
      else {
        hi = mid;
      }
    }
    prev[i] = lo ? tails[lo - 1] : (size_t)-1;
    tails[lo] = i;
    if (lo == len) {
      len++;
    }
  }
  if (len) {
    size_t i = tails[len - 1];
    for (;;) {
      keep[i] = true;
      if (prev[i] == (size_t)-1) {
        break;
      }
      i = prev[i];
    }
  }
  gtext_allocator_free(&b->alloc, tails);
  gtext_allocator_free(&b->alloc, prev);
  return true;
}

static bool order_remove_at(GTEXT_CSSOM_Markup_Binding * b, size_t i) {
  mentry * e = b->order[i];
  map_del(b, e);
  memmove(&b->order[i], &b->order[i + 1], (b->n - i - 1) * sizeof *b->order);
  b->n--;
  entry_free(b, e);
  return true;
}

static GTEXT_CSS_Status sync_walk(GTEXT_CSSOM_Markup_Binding * b, GTEXT_CSSOM_Outcome * outcome, uint64_t generation) {
  /* A. read: the elements that qualify, in tree order. Nothing is changed by this. */
  const GTEXT_Markup_Node * root = gtext_markup_document_root(b->markup);
  size_t ncand = 0, capcand = 0;
  cand * cands = NULL;
  b->counters.walks++;
  for (const GTEXT_Markup_Node * x = gtext_markup_node_next_in_order(root, root, false); x; x = gtext_markup_node_next_in_order(root, x, false)) {
    b->counters.nodes_visited++;
    bool link = false;
    if (gtext_markup_node_kind(x) == GTEXT_MARKUP_KIND_ELEMENT && qualifies(b, x, &link)) {
      if (ncand == capcand) {
        const size_t cap = capcand ? capcand * 2 : 16;
        cand * grown = gtext_allocator_realloc(&b->alloc, cands, cap * sizeof *grown);
        if (!grown) {
          gtext_allocator_free(&b->alloc, cands);
          return GTEXT_CSS_E_OOM;
        }
        cands = grown;
        capcand = cap;
      }
      cands[ncand].element = x;
      cands[ncand].link = link;
      ncand++;
    }
  }
  int64_t * seq = gtext_allocator_malloc(&b->alloc, (ncand ? ncand : 1) * sizeof *seq);
  bool * keep = gtext_allocator_malloc(&b->alloc, (ncand ? ncand : 1) * sizeof *keep);
  if (!seq || !keep) {
    gtext_allocator_free(&b->alloc, seq);
    gtext_allocator_free(&b->alloc, keep);
    gtext_allocator_free(&b->alloc, cands);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < b->n; i++) {
    b->order[i]->old_index = i;
    b->order[i]->stay = false;
  }
  for (size_t i = 0; i < ncand; i++) {
    mentry * e = map_find(b, cands[i].element);
    seq[i] = e && e->link == cands[i].link ? (int64_t)e->old_index : (int64_t)-1;
  }
  if (!lis_mark(b, seq, ncand, keep)) {
    gtext_allocator_free(&b->alloc, seq);
    gtext_allocator_free(&b->alloc, keep);
    gtext_allocator_free(&b->alloc, cands);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < ncand; i++) {
    if (keep[i]) {
      map_find(b, cands[i].element)->stay = true;
    }
  }
  gtext_allocator_free(&b->alloc, seq);
  gtext_allocator_free(&b->alloc, keep);
  /* B1. entries that are gone or moved lose their sheet */
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  for (size_t i = 0; i < b->n && st == GTEXT_CSS_OK;) {
    mentry * e = b->order[i];
    if (e->stay) {
      i++;
      continue;
    }
    st = take_out(b, e, outcome);
    if (st == GTEXT_CSS_OK) {
      order_remove_at(b, i);
      if (outcome) {
        outcome->kind = GTEXT_CSSOM_CHANGED;
      }
    }
  }
  /* B2. one pass: keep or refresh what is there, make what is new */
  size_t k = 0, hint = 0;
  for (size_t i = 0; i < ncand && st == GTEXT_CSS_OK; i++) {
    mentry * e;
    if (k < b->n && b->order[k]->element == cands[i].element) {
      e = b->order[k];
      st = refresh_entry(b, e, k, hint, outcome);
    }
    else {
      e = gtext_allocator_calloc(&b->alloc, 1, sizeof *e);
      if (!e || !order_reserve(b, 1)) {
        gtext_allocator_free(&b->alloc, e);
        st = GTEXT_CSS_E_OOM;
        break;
      }
      e->element = cands[i].element;
      e->link = cands[i].link;
      e->state = ENTRY_NEEDS;
      if (!map_add(b, e)) {
        gtext_allocator_free(&b->alloc, e);
        st = GTEXT_CSS_E_OOM;
        break;
      }
      memmove(&b->order[k + 1], &b->order[k], (b->n - k) * sizeof *b->order);
      b->order[k] = e;
      b->n++;
      st = build_entry(b, e, k, hint, outcome);
    }
    if (e->sheet) {
      size_t at = 0;
      if (locate(b, e->sheet, hint, &at)) {
        hint = at + 1;
      }
    }
    k++;
  }
  gtext_allocator_free(&b->alloc, cands);
  if (st == GTEXT_CSS_OK) {
    b->last_generation = generation;
    b->synced = true;
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_markup_sync(GTEXT_CSSOM_Markup_Binding * b, GTEXT_CSSOM_Outcome * outcome) {
  if (!b) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSSOM_Document * doc = document_of(b);
  if (outcome) {
    memset(outcome, 0, sizeof *outcome);
    outcome->kind = GTEXT_CSSOM_UNCHANGED;
    outcome->generation = doc ? gtext_cssom_document_generation(doc) : 0;
  }
  if (!doc) {
    return GTEXT_CSS_E_INVALID;
  }
  b->counters.syncs++;
  const uint64_t generation = gtext_markup_document_generation(b->markup);
  if (b->synced && generation == b->last_generation) {
    b->counters.shortcuts++;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Status st = gtext_cssom_document_begin_batch(doc);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Outcome inner;
  memset(&inner, 0, sizeof inner);
  inner.kind = GTEXT_CSSOM_UNCHANGED;
  st = sync_walk(b, &inner, generation);
  GTEXT_CSSOM_Outcome ended;
  memset(&ended, 0, sizeof ended);
  gtext_cssom_document_end_batch(doc, &ended);
  if (outcome) {
    outcome->kind = inner.kind;
    outcome->observer_status = inner.observer_status != GTEXT_CSS_OK ? inner.observer_status : ended.observer_status;
    outcome->generation = gtext_cssom_document_generation(doc);
  }
  return st;
}

/* ---- one element ------------------------------------------------------------------------------------------------------------------------------------- */

static bool connected(const GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * el) {
  const GTEXT_Markup_Node * root = gtext_markup_document_root(b->markup);
  for (const GTEXT_Markup_Node * x = el; x; x = gtext_markup_node_parent(x)) {
    if (x == root) {
      return true;
    }
  }
  return false;
}

/// The node before this one in document order (the deepest last descendant of the previous sibling, else the parent).
static const GTEXT_Markup_Node * previous_in_order(const GTEXT_Markup_Node * x) {
  const GTEXT_Markup_Node * p = gtext_markup_node_previous_sibling(x);
  if (!p) {
    return gtext_markup_node_parent(x);
  }
  for (const GTEXT_Markup_Node * c = gtext_markup_node_last_child(p); c; c = gtext_markup_node_last_child(p)) {
    p = c;
  }
  return p;
}

static size_t order_index_of(const GTEXT_CSSOM_Markup_Binding * b, const mentry * e) {
  for (size_t i = 0; i < b->n; i++) {
    if (b->order[i] == e) {
      return i;
    }
  }
  return b->n;
}

static GTEXT_CSS_Status element_step(GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * el, GTEXT_CSSOM_Outcome * outcome) {
  mentry * e = map_find(b, el);
  bool link = false;
  const bool wanted = connected(b, el) && qualifies(b, el, &link);
  if (!wanted && !e) {
    return GTEXT_CSS_OK;
  }
  if (!wanted || (e && e->link != link)) {
    GTEXT_CSS_Status st = take_out(b, e, outcome);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    order_remove_at(b, order_index_of(b, e));
    if (outcome) {
      outcome->kind = GTEXT_CSSOM_CHANGED;
    }
    if (!wanted) {
      return GTEXT_CSS_OK;
    }
    e = NULL;
  }
  if (e) {
    return refresh_entry(b, e, order_index_of(b, e), 0, outcome);
  }
  /* new: it goes after the nearest element before it that has an entry */
  size_t at = 0;
  for (const GTEXT_Markup_Node * x = previous_in_order(el); x; x = previous_in_order(x)) {
    mentry * p = map_find(b, x);
    if (p) {
      at = order_index_of(b, p) + 1;
      break;
    }
  }
  e = gtext_allocator_calloc(&b->alloc, 1, sizeof *e);
  if (!e || !order_reserve(b, 1)) {
    gtext_allocator_free(&b->alloc, e);
    return GTEXT_CSS_E_OOM;
  }
  e->element = el;
  e->link = link;
  e->state = ENTRY_NEEDS;
  if (!map_add(b, e)) {
    gtext_allocator_free(&b->alloc, e);
    return GTEXT_CSS_E_OOM;
  }
  memmove(&b->order[at + 1], &b->order[at], (b->n - at) * sizeof *b->order);
  b->order[at] = e;
  b->n++;
  return build_entry(b, e, at, 0, outcome);
}

GTEXT_CSS_Status gtext_cssom_markup_style_element_changed(GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * element, GTEXT_CSSOM_Outcome * outcome) {
  if (!b || !element) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSSOM_Document * doc = document_of(b);
  if (outcome) {
    memset(outcome, 0, sizeof *outcome);
    outcome->kind = GTEXT_CSSOM_UNCHANGED;
    outcome->generation = doc ? gtext_cssom_document_generation(doc) : 0;
  }
  if (!doc || gtext_markup_node_document(element) != (GTEXT_Markup_Document *)(uintptr_t)b->markup) {
    return GTEXT_CSS_E_INVALID;
  }
  b->counters.element_calls++;
  GTEXT_CSS_Status st = gtext_cssom_document_begin_batch(doc);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Outcome inner;
  memset(&inner, 0, sizeof inner);
  inner.kind = GTEXT_CSSOM_UNCHANGED;
  st = element_step(b, element, &inner);
  GTEXT_CSSOM_Outcome ended;
  memset(&ended, 0, sizeof ended);
  gtext_cssom_document_end_batch(doc, &ended);
  if (outcome) {
    outcome->kind = inner.kind;
    outcome->observer_status = inner.observer_status != GTEXT_CSS_OK ? inner.observer_status : ended.observer_status;
    outcome->generation = gtext_cssom_document_generation(doc);
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_markup_link_complete(GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * element, const GTEXT_CSSOM_Link_Text * text, GTEXT_CSSOM_Outcome * outcome) {
  if (!b || !element || !text || text->size < offsetof(GTEXT_CSSOM_Link_Text, base_url)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSSOM_Document * doc = document_of(b);
  if (outcome) {
    memset(outcome, 0, sizeof *outcome);
    outcome->kind = GTEXT_CSSOM_UNCHANGED;
  }
  if (!doc) {
    return GTEXT_CSS_E_INVALID;
  }
  mentry * e = map_find(b, element);
  if (!e || !e->link || e->state != ENTRY_PENDING) {
    return GTEXT_CSS_E_NOT_FOUND;
  }
  /* a change of the element since the request makes the answer stale */
  const char * fp = NULL;
  size_t fpn = 0;
  GTEXT_CSS_Status st = link_fingerprint(b, element, &fp, &fpn);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (fpn != e->text_length || memcmp(fp, e->text, fpn) != 0) {
    return GTEXT_CSS_OK;
  }
  st = gtext_cssom_document_begin_batch(doc);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Outcome inner;
  memset(&inner, 0, sizeof inner);
  inner.kind = GTEXT_CSSOM_UNCHANGED;
  const size_t at = order_index_of(b, e);
  GTEXT_CSSOM_Link_Text lt = *text;
  st = put_sheet(b, e, text->text ? text->text : "", text->length, &lt, insertion_index(b, at, 0), &inner);
  GTEXT_CSSOM_Outcome ended;
  memset(&ended, 0, sizeof ended);
  gtext_cssom_document_end_batch(doc, &ended);
  if (outcome) {
    outcome->kind = inner.kind;
    outcome->observer_status = inner.observer_status != GTEXT_CSS_OK ? inner.observer_status : ended.observer_status;
    outcome->generation = gtext_cssom_document_generation(doc);
  }
  return st;
}

GTEXT_CSSOM_Sheet * gtext_cssom_markup_sheet_of(const GTEXT_CSSOM_Markup_Binding * b, const GTEXT_Markup_Node * element) {
  if (!b || !element) {
    return NULL;
  }
  const mentry * e = map_find(b, element);
  return e && e->sheet ? gtext_cssom_sheet_retain(e->sheet) : NULL;
}

GTEXT_CSS_Status gtext_cssom_markup_binding_counters(const GTEXT_CSSOM_Markup_Binding * b, GTEXT_CSSOM_Markup_Counters * out) {
  if (!b || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = b->counters;
  out->entries = b->n;
  return GTEXT_CSS_OK;
}

/* ---- the `style` attribute --------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status inline_get(void * user, const void * element, const char ** value, size_t * length, bool * present) {
  (void)user;
  GTEXT_Markup_Attribute a;
  if (gtext_markup_element_get_attribute((const GTEXT_Markup_Node *)element, NULL, "style", &a)) {
    *value = a.value;
    *length = a.value_length;
    *present = true;
  }
  else {
    *value = "";
    *length = 0;
    *present = false;
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status inline_set(void * user, const void * element, const char * value, size_t length) {
  (void)user;
  return status_of(gtext_markup_element_set_attribute((GTEXT_Markup_Node *)(uintptr_t)element, NULL, NULL, "style", value, length));
}

static GTEXT_CSS_Status inline_remove(void * user, const void * element) {
  (void)user;
  gtext_markup_element_remove_attribute((GTEXT_Markup_Node *)(uintptr_t)element, NULL, "style");
  return GTEXT_CSS_OK;
}

GTEXT_CSSOM_Attribute_Binding gtext_cssom_markup_inline_binding(void) {
  GTEXT_CSSOM_Attribute_Binding b;
  memset(&b, 0, sizeof b);
  b.size = sizeof b;
  b.get_attribute = inline_get;
  b.set_attribute = inline_set;
  b.remove_attribute = inline_remove;
  return b;
}

uint64_t gtext_cssom_markup_tree_version(void * user) {
  return gtext_markup_document_generation((const GTEXT_Markup_Document *)user);
}
